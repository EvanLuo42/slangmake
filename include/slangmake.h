#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slangmake
{
enum class Target : uint32_t
{
    SPIRV,
    DXIL,
    DXBC,
    HLSL,
    GLSL,
    Metal,
    MetalLib,
    WGSL,
};

enum class Optimization : uint32_t
{
    None,
    Default,
    High,
    Maximal
};

enum class Codec : uint32_t
{
    None = 0,
    LZ4  = 1,
    Zstd = 2,
};

enum class MatrixLayout : uint32_t
{
    RowMajor,
    ColumnMajor
};

enum class FloatingPointMode : uint32_t
{
    Default,
    Fast,
    Precise
};

/**
 * Canonical lowercase name of a codec.
 *
 * @param c codec to stringify
 * @return  "none", "lz4", or "zstd"
 */
const char* codecToString(Codec c);

/**
 * Parse a codec name (case-insensitive; accepts "none"/""/"lz4"/"zstd"/"zst").
 *
 * @param s textual codec name
 * @return  the parsed codec, or std::nullopt if unrecognised
 */
std::optional<Codec> parseCodec(std::string_view s);

/**
 * Canonical name of a compile target.
 *
 * @param t target to stringify
 * @return  the canonical spelling ("SPIRV", "DXIL", ...)
 */
const char* targetToString(Target t);

/**
 * Parse a target name (case-insensitive; accepts "SPIR-V"/"SPV" aliases).
 *
 * @param s textual target name
 * @return  the parsed target, or std::nullopt if unrecognised
 */
std::optional<Target> parseTarget(std::string_view s);

struct ShaderConstant
{
    std::string name;
    std::string value;
};

struct PermutationDefine
{
    enum class Kind : uint32_t
    {
        Constant, // preprocessor #define (value is a literal/expression)
        Type      // generic type parameter (value is a Slang type expression)
    };
    std::string              name;
    std::vector<std::string> values;
    Kind                     kind = Kind::Constant;
};

struct VulkanBindShift
{
    uint32_t kind  = 0; // 's','t','b','u' as ASCII u32
    uint32_t space = 0;
    uint32_t shift = 0;
};

struct CompileOptions
{
    std::filesystem::path              inputFile;
    std::string                        entryPoint;
    Target                             target = Target::SPIRV;
    std::string                        profile;
    std::vector<std::filesystem::path> includePaths;
    std::vector<ShaderConstant>        defines;
    Optimization                       optimization      = Optimization::High;
    MatrixLayout                       matrixLayout      = MatrixLayout::RowMajor;
    FloatingPointMode                  fpMode            = FloatingPointMode::Default;
    bool                               debugInfo         = false;
    bool                               warningsAsErrors  = false;
    bool                               glslScalarLayout  = false;
    bool                               emitSpirvDirectly = true;
    bool                               dumpIntermediates = false;
    bool                               emitReflection    = true;
    int                                vulkanVersion     = 0;
    std::vector<VulkanBindShift>       vulkanBindShifts;
    std::string                        languageVersion;
};

struct Permutation
{
    std::vector<ShaderConstant> constants;
    // Bindings for module-scope `type_param T : IFoo;` parameters. Name matches
    // the type parameter's identifier; value is a Slang type expression passed
    // to IComponentType::specialize as an Expr SpecializationArg.
    std::vector<ShaderConstant> typeArgs;

    /**
     * Canonical permutation key. Names are sorted alphabetically so two
     * permutations with the same axes always produce the same key. When
     * typeArgs is non-empty it is appended after a '|' separator so the key is
     * backward-compatible with permutations that only use constants.
     *
     * @return the joined key, e.g. "A=0_B=1" or "A=0|T=Metal"; empty string
     *         when both vectors are empty
     */
    [[nodiscard]] std::string key() const;
};

/**
 * Parses "// [permutation] NAME={a,b,c}" magic-comment directives from Slang
 * source text. Block comments are ignored. Malformed lines are dropped silently.
 */
class PermutationParser
{
public:
    /**
     * Parse directives from an in-memory source buffer.
     *
     * @param source full slang source text
     * @return       one PermutationDefine per directive, in source order
     */
    static std::vector<PermutationDefine> parse(std::string_view source);

    /**
     * Parse directives from a .slang file on disk.
     *
     * @param path file system path to a slang source file
     * @return     directives found; empty if the file is missing or unreadable
     */
    static std::vector<PermutationDefine> parseFile(const std::filesystem::path& path);
};

/**
 * Expands a list of PermutationDefines into the Cartesian product of their
 * values, one Permutation per cell.
 */
class PermutationExpander
{
public:
    /**
     * Compute the Cartesian product of all directives' values.
     *
     * @param defs the permutation directives (typically after merge)
     * @return     one Permutation per cell; always at least one entry (empty
     *             permutation) when defs is empty
     */
    static std::vector<Permutation> expand(const std::vector<PermutationDefine>& defs);
};

/**
 * Merge CLI overrides on top of file-level directives. On name collision, the
 * CLI value wins and replaces the file value list entirely.
 *
 * @param fileDefines directives recovered from the source file
 * @param cliOverride user-supplied `-P NAME={...}` overrides
 * @return            the merged directive list; CLI entries appended at the end
 */
std::vector<PermutationDefine> mergePermutationDefines(const std::vector<PermutationDefine>& fileDefines,
                                                       const std::vector<PermutationDefine>& cliOverride);

/**
 * Scan a Slang source buffer for a `// [noreflection]` magic comment. Block
 * comments are ignored.
 *
 * @param source full slang source text
 * @return       true if the directive is present anywhere in the file
 */
bool sourceHasNoReflection(std::string_view source);

/**
 * File-path variant of sourceHasNoReflection.
 *
 * @param path file system path to a slang source file
 * @return     true if the file contains a `// [noreflection]` directive;
 *             false if the file is missing or unreadable
 */
bool fileHasNoReflection(const std::filesystem::path& path);

namespace fmt
{

constexpr uint32_t kBlobMagic         = 0x474E4C53; // 'SLNG'
constexpr uint32_t kBlobVersion       = 1;
constexpr uint32_t kReflectionMagic   = 0x46524C53; // 'SLRF'
constexpr uint32_t kReflectionVersion = 1;
constexpr uint32_t kInvalidIndex      = 0xFFFFFFFFu;

#pragma pack(push, 4)

struct BlobHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t target;
    uint32_t entryCount;
    uint32_t depsOffset;
    uint32_t depsCount;
    uint32_t depsStringsOffset;
    uint32_t depsStringsSize;
    uint32_t entryDepsIdxOffset; // u32[] shared pool of per-entry dep indices
    uint32_t entryDepsIdxCount;
    uint32_t compression;
    uint32_t uncompressedPayloadSize;
    uint64_t optionsHash;
};
static_assert(sizeof(BlobHeader) == 56);

struct DepEntry
{
    uint32_t pathOffset;
    uint32_t pathSize;
    uint64_t contentHash;
};
static_assert(sizeof(DepEntry) == 16);

struct EntryRecord
{
    uint32_t keyOffset;
    uint32_t keySize;
    uint32_t codeOffset;
    uint32_t codeSize;
    uint32_t reflOffset;
    uint32_t reflSize;
    uint32_t depsIdxOff;   // into BlobHeader::entryDepsIdx pool
    uint32_t depsIdxCount; // number of u32 indices into DepEntry[]
};
static_assert(sizeof(EntryRecord) == 32);

struct ReflHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t flags;

    uint32_t entryPointHashLow;
    uint32_t entryPointHashHigh;
    uint32_t globalCbBinding;
    uint32_t globalCbSize;
    uint32_t globalParamsVarLayoutIdx;
    uint32_t bindlessSpaceIndex;

    uint32_t strings_off, strings_size;
    uint32_t hashedStr_off, hashedStr_count;
    uint32_t attrArg_off, attrArg_count;
    uint32_t attr_off, attr_count;
    uint32_t modifier_off, modifier_count;
    uint32_t type_off, type_count;
    uint32_t typeLayout_off, typeLayout_count;
    uint32_t var_off, var_count;
    uint32_t varLayout_off, varLayout_count;
    uint32_t func_off, func_count;
    uint32_t generic_off, generic_count;
    uint32_t decl_off, decl_count;
    uint32_t entryPoint_off, entryPoint_count;
    uint32_t bindingRange_off, bindingRange_count;
    uint32_t descriptorSet_off, descriptorSet_count;
    uint32_t descriptorRange_off, descriptorRange_count;
    uint32_t subObjectRange_off, subObjectRange_count;
    uint32_t u32Pool_off, u32Pool_count;
};

struct HashedStr
{
    uint32_t strIdx;
    uint32_t hash;
};

struct AttrArg
{
    enum Kind : uint32_t
    {
        None   = 0,
        Int    = 1,
        Float  = 2,
        String = 3
    };
    uint32_t kind;
    uint32_t strIdx;
    uint64_t raw;
};

struct Attribute
{
    uint32_t nameStrIdx;
    uint32_t argOff;
    uint32_t argCount;
};

struct Type
{
    uint32_t kind;
    uint32_t nameStrIdx;
    uint32_t fullNameStrIdx;
    uint32_t scalarKind;
    uint32_t fieldCount;
    uint32_t fieldPoolOff;
    uint32_t rowCount;
    uint32_t colCount;
    uint32_t elementTypeIdx;
    uint32_t elementCount;
    uint32_t totalArrayElementCount;
    uint32_t resourceShape;
    uint32_t resourceAccess;
    uint32_t resourceResultTypeIdx;
};

struct TypeLayout
{
    uint32_t typeIdx;
    uint32_t kind;
    uint32_t parameterCategory;
    uint32_t categoryCount;
    uint32_t categoryPoolOff;
    uint32_t fieldCount;
    uint32_t fieldLayoutPoolOff;
    uint32_t containerVarLayoutIdx;
    uint32_t elementTypeLayoutIdx;
    uint32_t elementVarLayoutIdx;
    uint32_t explicitCounterVarLayoutIdx;
    uint32_t matrixLayoutMode;
    uint32_t genericParamIndex;
    uint32_t sizePoolOff;
    uint32_t bindingRangeCount;
    uint32_t bindingRangeOff;
    uint32_t descriptorSetCount;
    uint32_t descriptorSetOff;
    uint32_t subObjectRangeCount;
    uint32_t subObjectRangeOff;
};

struct BindingRange
{
    uint32_t bindingType;
    uint32_t bindingCount;
    uint32_t leafTypeLayoutIdx;
    uint32_t leafVariableIdx;
    uint32_t imageFormat;
    uint32_t isSpecializable;
    uint32_t descriptorSetIndex;
    uint32_t firstDescriptorRangeIndex;
    uint32_t descriptorRangeCount;
};

struct DescriptorSet
{
    uint32_t spaceOffset;
    uint32_t descriptorRangeStart;
    uint32_t descriptorRangeCount;
    uint32_t pad;
};

struct DescriptorRange
{
    uint32_t indexOffset;
    uint32_t descriptorCount;
    uint32_t bindingType;
    uint32_t parameterCategory;
};

struct SubObjectRange
{
    uint32_t bindingRangeIndex;
    uint32_t spaceOffset;
    uint32_t offsetVarLayoutIdx;
};

struct Variable
{
    uint32_t nameStrIdx;
    uint32_t typeIdx;
    uint32_t modifierOff;
    uint32_t modifierCount;
    uint32_t attrOff;
    uint32_t attrCount;
    uint32_t hasDefault;
    uint32_t defaultKind; // 0=none, 1=int, 2=float
    uint64_t defaultValueRaw;
    uint32_t genericContainerIdx;
};

struct VarLayout
{
    uint32_t varIdx;
    uint32_t typeLayoutIdx;
    uint32_t category;
    uint32_t categoryCount;
    uint32_t categoryPoolOff;
    uint32_t offsetPoolOff;
    uint32_t bindingIndex;
    uint32_t bindingSpacePoolOff;
    uint32_t imageFormat;
    uint32_t semanticNameStrIdx;
    uint32_t semanticIndex;
    uint32_t stage;
    uint32_t modifierOff;
    uint32_t modifierCount;
    uint32_t attrOff;
    uint32_t attrCount;
};

struct Function
{
    uint32_t nameStrIdx;
    uint32_t returnTypeIdx;
    uint32_t paramVarOff;
    uint32_t paramVarCount;
    uint32_t modifierOff;
    uint32_t modifierCount;
    uint32_t attrOff;
    uint32_t attrCount;
    uint32_t genericContainerIdx;
};

struct Generic
{
    uint32_t nameStrIdx;
    uint32_t innerDeclIdx;
    uint32_t innerKind;
    uint32_t typeParamOff;
    uint32_t typeParamCount;
    uint32_t valueParamOff;
    uint32_t valueParamCount;
    uint32_t constraintPoolOff; // (typeParamIdx, constraintTypeIdx) pairs
    uint32_t constraintCount;
    uint32_t outerGenericIdx;
};

struct Decl
{
    uint32_t kind;
    uint32_t nameStrIdx;
    uint32_t parentDeclIdx;
    uint32_t childOff;
    uint32_t childCount;
    uint32_t payloadIdx;
};

struct EntryPoint
{
    uint32_t nameStrIdx;
    uint32_t nameOverrideStrIdx;
    uint32_t stage;
    uint32_t threadGroupSizeX;
    uint32_t threadGroupSizeY;
    uint32_t threadGroupSizeZ;
    uint32_t waveSize;
    uint32_t usesAnySampleRateInput;
    uint32_t hasDefaultConstantBuffer;
    uint32_t varLayoutIdx;
    uint32_t typeLayoutIdx;
    uint32_t resultVarLayoutIdx;
    uint32_t functionIdx;
    uint32_t paramVarLayoutOff;
    uint32_t paramVarLayoutCount;
    uint32_t attrOff;
    uint32_t attrCount;
    uint32_t hashLow;
    uint32_t hashHigh;
};

#pragma pack(pop)

} // namespace fmt

/**
 * Compiles a single permutation of a .slang source on demand. Internally owns
 * a Slang global session, but the Slang dependency is hidden behind a PImpl so
 * `slangmake.h` consumers don't need slang headers / DLLs unless they actually
 * instantiate Compiler. One Compiler can be reused across many compile() calls.
 */
class Compiler
{
public:
    Compiler();
    ~Compiler();
    Compiler(const Compiler&)            = delete;
    Compiler& operator=(const Compiler&) = delete;

    struct Result
    {
        bool                     success = false;
        std::vector<uint8_t>     code;
        std::vector<uint8_t>     reflection;
        std::string              diagnostics;
        std::vector<std::string> dependencies; // absolute paths reported by IModule
    };

    /**
     * Compile a single permutation to target bytecode + packed reflection.
     *
     * @param opts compile-wide options (target, profile, includes, defines, ...)
     * @param perm the permutation-specific defines overlaid on top of opts.defines
     * @return     per-call Result with success flag, bytecode, reflection blob,
     *             accumulated diagnostics and the list of dependency file paths
     *             that slang saw during front-end parsing
     */
    Result compile(const CompileOptions& opts, const Permutation& perm) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

/**
 * Packs compiled entries into a single SLNG blob. Append entries with
 * addEntry(), optionally set dependency + options-hash + compression metadata,
 * then finalize() or writeToFile().
 */
class BlobWriter
{
public:
    explicit BlobWriter(Target target);

    /**
     * Append one compiled permutation to the blob.
     *
     * @param perm        permutation whose key() becomes the entry identifier
     * @param code        target bytecode for the permutation
     * @param reflection  packed reflection section, or an empty span to skip
     * @param depIndices  indices into the blob-level dependency list
     *                    (the one set via setDependencies) identifying the
     *                    files this permutation actually read. Empty means
     *                    "unknown / don't participate in per-entry reuse".
     */
    void addEntry(const Permutation& perm, std::span<const uint8_t> code, std::span<const uint8_t> reflection,
                  std::span<const uint32_t> depIndices = {});

    struct DepInfo
    {
        std::string path;
        uint64_t    contentHash;
    };

    void setDependencies(std::vector<DepInfo> deps) { m_deps = std::move(deps); }
    void setOptionsHash(uint64_t h) { m_optionsHash = h; }
    void setCompression(Codec c) { m_compression = c; }

    /**
     * Serialise everything queued so far into a contiguous blob.
     *
     * @return the in-memory blob bytes; size depends on selected compression
     */
    std::vector<uint8_t> finalize() const;

    /**
     * Serialise to disk. Creates parent directories as needed.
     *
     * @param path destination file; overwritten if it exists
     */
    void writeToFile(const std::filesystem::path& path) const;

    size_t entryCount() const { return m_entries.size(); }

private:
    Target m_target;
    struct Entry
    {
        std::string           key;
        std::vector<uint8_t>  code;
        std::vector<uint8_t>  refl;
        std::vector<uint32_t> depIndices;
    };
    std::vector<Entry>   m_entries;
    std::vector<DepInfo> m_deps;
    uint64_t             m_optionsHash = 0;
    Codec                m_compression = Codec::None;
};

/**
 * Zero-copy reader over a packed SLNG blob. Entry code and reflection are
 * returned as spans into the in-memory buffer. If the blob is compressed, the
 * reader decompresses once at construction into an owned buffer and exposes
 * spans into that buffer thereafter.
 */
class BlobReader
{
public:
    /**
     * Non-owning view. The caller must keep the underlying buffer alive for
     * the lifetime of the BlobReader.
     */
    explicit BlobReader(std::span<const uint8_t> blob);

    /**
     * Take ownership of the buffer. Safe to use with rvalue finalize() output.
     */
    explicit BlobReader(std::vector<uint8_t>&& blob);

    /**
     * Construct a BlobReader by memory-loading a file from disk.
     *
     * @param path path to a .bin produced by BlobWriter
     * @return     reader on success, std::nullopt on I/O or parse failure
     */
    static std::optional<BlobReader> openFile(const std::filesystem::path& path);

    bool   valid() const { return m_hdr != nullptr; }
    Target target() const;
    size_t entryCount() const;

    struct Entry
    {
        std::string_view          key;
        std::span<const uint8_t>  code;
        std::span<const uint8_t>  reflection;
        std::span<const uint32_t> depIndices; // indices into dependencies()
    };

    /**
     * Retrieve the Nth entry by index.
     *
     * @param index entry index in [0, entryCount())
     * @return      spans over key/code/reflection; empty spans on out-of-range
     */
    Entry at(size_t index) const;

    /**
     * Find the entry whose key matches the alphabetical join of `constants`.
     *
     * @param constants permutation-define name/value pairs in any order
     * @return          matching entry or std::nullopt
     */
    std::optional<Entry> find(std::span<const ShaderConstant> constants) const;

    /**
     * Find the entry whose key matches `perm.key()` verbatim. Use this overload
     * when the permutation carries type-argument axes (`perm.typeArgs`) in
     * addition to preprocessor constants.
     *
     * @param perm full permutation, constants and type arguments combined
     * @return     matching entry or std::nullopt
     */
    std::optional<Entry> find(const Permutation& perm) const;

    /**
     * List every entry key in insertion order.
     *
     * @return the key of every entry in the blob
     */
    std::vector<std::string> enumerate() const;

    struct Dep
    {
        std::string_view path;
        uint64_t         contentHash;
    };

    std::vector<Dep> dependencies() const;
    uint64_t         optionsHash() const;
    Codec            compression() const;

private:
    std::span<const uint8_t> m_blob;
    std::vector<uint8_t>     m_owned;
    const fmt::BlobHeader*   m_hdr     = nullptr;
    const fmt::EntryRecord*  m_entries = nullptr;

    void rebind(std::span<const uint8_t> blob);
};

/**
 * Typed zero-copy view over a reflection section produced by the serializer.
 * Every table accessor returns a std::span into the underlying bytes; the
 * decoded* helpers walk the tables and materialise small convenience structs.
 */
class ReflectionView
{
public:
    explicit ReflectionView(std::span<const uint8_t> bytes);

    bool valid() const { return m_hdr != nullptr; }

    uint64_t                firstEntryPointHash() const;
    uint32_t                globalConstantBufferBinding() const;
    uint32_t                globalConstantBufferSize() const;
    uint32_t                bindlessSpaceIndex() const;
    std::optional<uint32_t> globalParamsVarLayout() const;

    std::span<const fmt::Type>            types() const;
    std::span<const fmt::TypeLayout>      typeLayouts() const;
    std::span<const fmt::Variable>        variables() const;
    std::span<const fmt::VarLayout>       varLayouts() const;
    std::span<const fmt::Function>        functions() const;
    std::span<const fmt::Generic>         generics() const;
    std::span<const fmt::Decl>            decls() const;
    std::span<const fmt::EntryPoint>      entryPoints() const;
    std::span<const fmt::Attribute>       attributes() const;
    std::span<const fmt::AttrArg>         attributeArgs() const;
    std::span<const uint32_t>             modifierPool() const;
    std::span<const fmt::HashedStr>       hashedStrings() const;
    std::span<const fmt::BindingRange>    bindingRanges() const;
    std::span<const fmt::DescriptorSet>   descriptorSets() const;
    std::span<const fmt::DescriptorRange> descriptorRanges() const;
    std::span<const fmt::SubObjectRange>  subObjectRanges() const;
    std::span<const uint32_t>             u32Pool() const;

    /**
     * Resolve a string-pool index into an interned name.
     *
     * @param strIdx index into the reflection string pool
     * @return       the interned NUL-terminated name; empty for kInvalidIndex
     *               or out-of-range indices
     */
    std::string_view string(uint32_t strIdx) const;

    /**
     * Look up one of the `hashedStrings()` entries by its table index.
     *
     * @param index 0-based index into hashedStrings()
     * @return      the interned name; empty view on out-of-range
     */
    std::string_view hashedString(uint32_t index) const;

    struct Param
    {
        std::string_view name;
        uint32_t         category;
        uint32_t         space;
        uint32_t         binding;
        uint32_t         byteOffset;
        uint32_t         byteSize;
        uint32_t         elementCount;
        uint32_t         imageFormat;
        std::string_view semanticName;
        uint32_t         semanticIndex;
        uint32_t         stage;
    };

    struct EntryPointInfo
    {
        std::string_view        name;
        std::string_view        nameOverride;
        uint32_t                stage;           // from [shader("...")]
        std::array<uint32_t, 3> threadGroupSize; // from [numthreads(...)]
        uint32_t                waveSize;
        uint64_t                hash;
        std::vector<Param>      parameters;
        /**
         * User-defined attributes only. Slang reports built-in ones like
         * `[shader("...")]` and `[numthreads(...)]` via dedicated reflection
         * calls (surfaced here as `stage` and `threadGroupSize`) rather than
         * through getUserAttribute*, so they will NOT appear in this list.
         */
        std::vector<std::pair<std::string_view, std::vector<fmt::AttrArg>>> attributes;
    };

    /**
     * Decode every entry point and its parameters into a convenient struct.
     *
     * @return one EntryPointInfo per entry point recorded in the reflection
     */
    std::vector<EntryPointInfo> decodedEntryPoints() const;

    /**
     * Decode the global parameter block (shader-wide resources) into Params.
     *
     * @return one Param per global parameter of the program
     */
    std::vector<Param> decodedGlobalParameters() const;

    struct DeclNode
    {
        uint32_t                  kind;
        std::string_view          name;
        std::span<const uint32_t> children;
        uint32_t                  payloadIdx;
        uint32_t                  parentDeclIdx;
    };

    std::optional<DeclNode> rootDecl() const;
    DeclNode                decl(uint32_t idx) const;

    /**
     * Slang-ShaderCursor-style navigator over the serialized reflection tree.
     *
     * A Cursor is a value type pointing at a logical position inside a shader
     * program's parameter block. Its primary operations are named/indexed
     * traversal (`operator[]`) and query of the accumulated binding state at
     * the current leaf (uniform byte offset, descriptor set/slot, per-category
     * offsets).
     *
     * Container types — `ConstantBuffer<T>`, `ParameterBlock<T>`,
     * `TextureBuffer<T>`, `ShaderStorageBuffer` — are auto-dereferenced on
     * navigation so `root["cb"]["field"]` works the same as
     * `root["cb"].dereference()["field"]`. Call `dereference()` explicitly only
     * when you need the container layer itself (e.g. to bind the implicit
     * constant buffer resource of a ParameterBlock).
     *
     * Offsets / spaces are tracked per SlangParameterCategory. A navigation
     * step accumulates every category present on the visited VariableLayout,
     * so a final Cursor's `offsetFor(Uniform)` is the full byte offset into
     * its owning constant buffer, `spaceFor(DescriptorTableSlot)` is the
     * final Vulkan set index, and so on — no caller-side arithmetic needed.
     *
     * The Cursor is non-owning and cheap to copy (~ 256 bytes).
     */
    class Cursor
    {
    public:
        Cursor() = default;

        bool valid() const { return m_rv != nullptr && m_typeLayoutIdx != fmt::kInvalidIndex; }

        /**
         * Navigate into a struct field by name. If the current type is a
         * container (CB/PB/TextureBuffer/SSB), it is auto-dereferenced first.
         *
         * @param name field identifier
         * @return     cursor at the field, or an invalid cursor if the name
         *             doesn't resolve or the current type is not a struct
         */
        Cursor field(std::string_view name) const;

        /**
         * Navigate into an array element by index. Strides come from the
         * element type's per-category size pool, so this works for arrays of
         * uniforms, descriptor-bound resources, and anything else Slang lays
         * out in a strided fashion.
         *
         * @param index element index (not bounds-checked against the array
         *              length — caller is responsible for staying in range)
         * @return      cursor at the element, or invalid if not an array
         */
        Cursor element(uint32_t index) const;

        Cursor operator[](std::string_view name) const { return field(name); }
        Cursor operator[](const char* name) const { return field(name); }
        Cursor operator[](uint32_t index) const { return element(index); }

        /**
         * Explicitly step into a container type's element layout. Rarely
         * needed because `field()` / `element()` auto-dereference; useful when
         * you want to bind the container layer itself (e.g. the implicit CB
         * behind a ParameterBlock).
         */
        Cursor dereference() const;

        /**
         * Inverse of `dereference()`: step onto the container layer of a
         * ConstantBuffer<T> / ParameterBlock<T> / TextureBuffer<T> /
         * ShaderStorageBuffer. The returned cursor represents the *container
         * resource itself* (the implicit constant buffer of a PB, or the CB
         * of a ConstantBuffer<T>), so `resourceBinding()` on it gives the
         * (space, slot) where the engine must write the VkBuffer /
         * ID3D12Resource handle.
         *
         * Returns an invalid cursor when the current type is not a container.
         */
        Cursor container() const;

        /**
         * Cursor for the explicit counter of an `AppendStructuredBuffer` /
         * `ConsumeStructuredBuffer`. Invalid on anything else.
         */
        Cursor explicitCounter() const;

        /**
         * Name of the VariableLayout that produced this cursor (i.e. the name
         * used in the last `field()` step). Empty when the cursor is at the
         * root or at an array element.
         */
        std::string_view name() const;

        uint32_t typeLayoutIndex() const { return m_typeLayoutIdx; }
        uint32_t typeKind() const; // slang::TypeReflection::Kind value

        /**
         * Accumulated offset along a single SlangParameterCategory axis.
         *
         * @param category SLANG_PARAMETER_CATEGORY_* value
         * @return         accumulated offset (byte count for Uniform / push
         *                 constants, slot index for DescriptorTableSlot /
         *                 D3D-style register categories)
         */
        uint32_t offsetFor(uint32_t category) const;

        /**
         * Accumulated space along a single SlangParameterCategory axis.
         * For most categories this is the descriptor-set / register-space
         * index relative to the program root.
         */
        uint32_t spaceFor(uint32_t category) const;

        /**
         * Size at the current cursor's type along a category axis, read from
         * the serialized TypeLayout sizePool. Returns 0 when the category
         * doesn't participate in the current type's layout.
         */
        uint32_t sizeFor(uint32_t category) const;
        uint32_t strideFor(uint32_t category) const;

        // Convenience shortcuts for the three categories nearly every engine
        // bind path cares about. Equivalent to offsetFor/spaceFor with the
        // corresponding SlangParameterCategory constants.
        uint32_t uniformOffset() const;
        uint32_t descriptorSlot() const;
        uint32_t descriptorSet() const;

        struct UniformLocation
        {
            uint32_t offsetBytes;
            uint32_t sizeBytes;
        };
        /**
         * Byte range `[offsetBytes, offsetBytes + sizeBytes)` into the
         * enclosing constant buffer that this cursor covers. Valid only when
         * the cursor sits in a uniform-bearing part of the layout; otherwise
         * sizeBytes is 0.
         */
        UniformLocation uniformLocation() const;

        struct ResourceBinding
        {
            uint32_t space;
            uint32_t index;
            uint32_t category; // SlangParameterCategory that supplied the binding
        };
        /**
         * First non-uniform binding seen on this cursor: (space, index,
         * category). Returns std::nullopt when the cursor is at a uniform-only
         * leaf (e.g. a scalar inside a CB) or at an invalid position.
         */
        std::optional<ResourceBinding> resourceBinding() const;

        // One descriptor range inside an enumerated descriptor set. Matches
        // Slang's own (DescriptorSet, DescriptorRange) split: a single set
        // may contain several ranges of different binding types.
        struct DescriptorBindingInfo
        {
            uint32_t slot;        // binding index within the set
            uint32_t count;       // descriptor count (array size; large for bindless)
            uint32_t bindingType; // SlangBindingType (SLANG_BINDING_TYPE_*)
            uint32_t category;    // SlangParameterCategory
        };

        struct DescriptorSetInfo
        {
            uint32_t                           space; // absolute descriptor-set / register-space index
            std::vector<DescriptorBindingInfo> bindings;
        };

        /**
         * Enumerate every descriptor set that belongs to the sub-tree rooted
         * at this cursor. For a cursor on a ParameterBlock, these are the
         * sets the PB needs — enough to build `VkDescriptorSetLayout` objects
         * or a D3D12 root signature slice. For the root cursor on a global
         * struct, these are the program-wide global sets.
         *
         * The returned `space` is absolute (cursor's accumulated space plus
         * the set's internal offset), ready to hand to vkCmdBindDescriptorSets.
         */
        std::vector<DescriptorSetInfo> descriptorSetLayout() const;

    private:
        friend class ReflectionView;

        // 32 slots cover every SlangParameterCategory value currently defined
        // (the enum runs through the low 20s). Trivially copyable.
        static constexpr size_t kMaxCategory = 32;

        const ReflectionView*              m_rv            = nullptr;
        uint32_t                           m_typeLayoutIdx = fmt::kInvalidIndex;
        uint32_t                           m_varLayoutIdx  = fmt::kInvalidIndex;
        std::array<uint32_t, kMaxCategory> m_offsets{};
        std::array<uint32_t, kMaxCategory> m_spaces{};

        void applyVarLayout(uint32_t vlIdx);
        void autoDereference();
    };

    /**
     * Cursor pointing at the program's global parameter block. From here,
     * `operator[]` walks into any global resource or uniform field by name.
     *
     * @return an invalid cursor if the blob carries no global-params layout.
     */
    Cursor rootCursor() const;

    /**
     * Cursor pointing at the parameter block of entry point `entryPointIndex`.
     * Entry-point uniforms are usually laid out in the entry-point's implicit
     * constant buffer; use this cursor to address them.
     *
     * @param entryPointIndex index into entryPoints()
     * @return                invalid cursor when the index is out of range
     */
    Cursor entryPointCursor(uint32_t entryPointIndex) const;

    /**
     * Convenience wrapper for `rootCursor()[name]`.
     */
    Cursor findGlobalParam(std::string_view name) const;

private:
    std::span<const uint8_t> m_bytes;
    const fmt::ReflHeader*   m_hdr = nullptr;

    Param decodeParam(const fmt::VarLayout& vl) const;
};

/**
 * High-level orchestration: parse permutation directives, expand them, drive
 * one or more Compiler instances (in parallel when requested), optionally
 * reuse entries from a previously-written blob, and pack the results.
 */
class BatchCompiler
{
public:
    explicit BatchCompiler(Compiler& compiler);

    struct Input
    {
        std::filesystem::path          file;
        CompileOptions                 options;
        std::vector<PermutationDefine> cliOverride;
    };

    struct Output
    {
        std::filesystem::path    outputFile;
        std::vector<uint8_t>     blob;
        std::vector<Permutation> compiled;
        std::vector<std::string> failures;
    };

    /**
     * Compile one .slang file to one .bin blob.
     *
     * @param in      batch input (file, options, CLI overrides)
     * @param outPath destination blob path
     * @return        compiled permutations, failures, and the serialised blob
     */
    Output compileFile(const Input& in, const std::filesystem::path& outPath);

    /**
     * Recursively compile every .slang file under `root`, mirroring the tree
     * into `outDir` with .bin extensions.
     *
     * @param root        input directory to walk
     * @param base        options applied to every file found
     * @param outDir      output directory root (created if missing)
     * @param cliOverride permutation overrides applied uniformly to every file
     * @return            one Output per discovered .slang source
     */
    std::vector<Output> compileDirectory(const std::filesystem::path& root, const CompileOptions& base,
                                         const std::filesystem::path&          outDir,
                                         const std::vector<PermutationDefine>& cliOverride = {});

    void setKeepGoing(bool v) { m_keepGoing = v; }
    void setVerbose(bool v) { m_verbose = v; }
    void setQuiet(bool v) { m_quiet = v; }
    void setJobs(int n) { m_jobs = n < 1 ? 1 : n; }
    void setIncremental(bool v) { m_incremental = v; }
    void setCompression(Codec c) { m_compression = c; }

    struct Stats
    {
        size_t reused   = 0;
        size_t compiled = 0;
    };

    /**
     * Stats from the last compileFile() call.
     *
     * @return reference to the internal stats: how many entries were reused
     *         from an existing blob versus compiled fresh this run
     */
    const Stats& lastStats() const { return m_stats; }

private:
    Compiler& m_compiler;
    bool      m_keepGoing   = false;
    bool      m_verbose     = false;
    bool      m_quiet       = false;
    bool      m_incremental = true;
    int       m_jobs        = 1;
    Codec     m_compression = Codec::None;
    Stats     m_stats;
};

} // namespace slangmake
