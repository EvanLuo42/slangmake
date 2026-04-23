#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <unordered_set>

#include "slangmake_internal.h"

namespace slangmake
{

const char* targetToString(Target t)
{
    switch (t)
    {
    case Target::SPIRV:
        return "SPIRV";
    case Target::DXIL:
        return "DXIL";
    case Target::DXBC:
        return "DXBC";
    case Target::HLSL:
        return "HLSL";
    case Target::GLSL:
        return "GLSL";
    case Target::Metal:
        return "Metal";
    case Target::MetalLib:
        return "MetalLib";
    case Target::WGSL:
        return "WGSL";
    }
    return "UNKNOWN";
}

std::optional<Target> parseTarget(std::string_view s)
{
    std::string up;
    up.reserve(s.size());
    for (char c : s)
        up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (up == "SPIRV" || up == "SPIR-V" || up == "SPV")
        return Target::SPIRV;
    if (up == "DXIL")
        return Target::DXIL;
    if (up == "DXBC")
        return Target::DXBC;
    if (up == "HLSL")
        return Target::HLSL;
    if (up == "GLSL")
        return Target::GLSL;
    if (up == "METAL")
        return Target::Metal;
    if (up == "METALLIB")
        return Target::MetalLib;
    if (up == "WGSL")
        return Target::WGSL;
    return std::nullopt;
}

const char* codecToString(Codec c)
{
    switch (c)
    {
    case Codec::None:
        return "none";
    case Codec::LZ4:
        return "lz4";
    case Codec::Zstd:
        return "zstd";
    }
    return "none";
}

std::optional<Codec> parseCodec(std::string_view s)
{
    std::string low;
    low.reserve(s.size());
    for (char ch : s)
        low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    if (low == "none" || low.empty())
        return Codec::None;
    if (low == "lz4")
        return Codec::LZ4;
    if (low == "zstd" || low == "zst")
        return Codec::Zstd;
    return std::nullopt;
}

SlangCompileTarget toSlangCompileTarget(Target t)
{
    switch (t)
    {
    case Target::SPIRV:
        return SLANG_SPIRV;
    case Target::DXIL:
        return SLANG_DXIL;
    case Target::DXBC:
        return SLANG_DXBC;
    case Target::HLSL:
        return SLANG_HLSL;
    case Target::GLSL:
        return SLANG_GLSL;
    case Target::Metal:
        return SLANG_METAL;
    case Target::MetalLib:
        return SLANG_METAL_LIB;
    case Target::WGSL:
        return SLANG_WGSL;
    }
    return SLANG_TARGET_UNKNOWN;
}

std::string Permutation::key() const
{
    auto joinSorted = [](std::vector<ShaderConstant> v) -> std::string
    {
        std::ranges::sort(v, [](const ShaderConstant& a, const ShaderConstant& b) { return a.name < b.name; });
        std::string s;
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (i)
                s.push_back('_');
            s += v[i].name;
            s.push_back('=');
            s += v[i].value;
        }
        return s;
    };

    std::string out = joinSorted(constants);
    if (!typeArgs.empty())
    {
        // Leading '|' when there are no constants distinguishes an all-type-args
        // key from an all-constants key that happens to use the same names.
        out.push_back('|');
        out += joinSorted(typeArgs);
    }
    return out;
}

namespace
{

bool isIdentStart(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool isIdentCont(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }

std::string trim(std::string_view s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        --b;
    return std::string(s.substr(a, b - a));
}

std::optional<size_t> findDirectiveValueListEnd(std::string_view s, size_t start, bool allowAngleNesting)
{
    int  angleDepth   = 0;
    int  parenDepth   = 0;
    int  bracketDepth = 0;
    int  braceDepth   = 0;
    char quote        = '\0';
    bool escape       = false;

    for (size_t i = start; i < s.size(); ++i)
    {
        const char ch = s[i];
        if (quote)
        {
            if (escape)
            {
                escape = false;
                continue;
            }
            if (ch == '\\')
            {
                escape = true;
                continue;
            }
            if (ch == quote)
                quote = '\0';
            continue;
        }

        switch (ch)
        {
        case '"':
        case '\'':
            quote = ch;
            break;
        case '<':
            if (allowAngleNesting)
                ++angleDepth;
            break;
        case '>':
            if (allowAngleNesting && angleDepth > 0)
                --angleDepth;
            break;
        case '(':
            ++parenDepth;
            break;
        case ')':
            if (parenDepth == 0)
                return std::nullopt;
            --parenDepth;
            break;
        case '[':
            ++bracketDepth;
            break;
        case ']':
            if (bracketDepth == 0)
                return std::nullopt;
            --bracketDepth;
            break;
        case '{':
            ++braceDepth;
            break;
        case '}':
            if (braceDepth == 0)
                return (angleDepth == 0 && parenDepth == 0 && bracketDepth == 0) ? std::optional<size_t>(i)
                                                                                 : std::nullopt;
            --braceDepth;
            break;
        default:
            break;
        }
    }

    return std::nullopt;
}

} // namespace

std::optional<std::vector<std::string>> detail::parsePermutationValueList(std::string_view inside,
                                                                          bool             allowAngleNesting)
{
    std::vector<std::string> values;
    size_t                   tokenStart   = 0;
    int                      angleDepth   = 0;
    int                      parenDepth   = 0;
    int                      bracketDepth = 0;
    int                      braceDepth   = 0;
    char                     quote        = '\0';
    bool                     escape       = false;

    auto flushToken = [&](size_t tokenEnd)
    {
        std::string t = trim(inside.substr(tokenStart, tokenEnd - tokenStart));
        if (!t.empty())
            values.push_back(std::move(t));
    };

    for (size_t i = 0; i < inside.size(); ++i)
    {
        const char ch = inside[i];
        if (quote)
        {
            if (escape)
            {
                escape = false;
                continue;
            }
            if (ch == '\\')
            {
                escape = true;
                continue;
            }
            if (ch == quote)
                quote = '\0';
            continue;
        }

        switch (ch)
        {
        case '"':
        case '\'':
            quote = ch;
            break;
        case '<':
            if (allowAngleNesting)
                ++angleDepth;
            break;
        case '>':
            if (allowAngleNesting && angleDepth > 0)
                --angleDepth;
            break;
        case '(':
            ++parenDepth;
            break;
        case ')':
            if (parenDepth == 0)
                return std::nullopt;
            --parenDepth;
            break;
        case '[':
            ++bracketDepth;
            break;
        case ']':
            if (bracketDepth == 0)
                return std::nullopt;
            --bracketDepth;
            break;
        case '{':
            ++braceDepth;
            break;
        case '}':
            if (braceDepth == 0)
                return std::nullopt;
            --braceDepth;
            break;
        case ',':
            if (angleDepth == 0 && parenDepth == 0 && bracketDepth == 0 && braceDepth == 0)
            {
                flushToken(i);
                tokenStart = i + 1;
            }
            break;
        default:
            break;
        }
    }

    if (quote || angleDepth != 0 || parenDepth != 0 || bracketDepth != 0 || braceDepth != 0)
        return std::nullopt;

    flushToken(inside.size());
    return values;
}

std::vector<PermutationDefine> PermutationParser::parse(std::string_view source)
{
    std::vector<PermutationDefine> out;

    // Strip block comments first so the line scan stays trivial.
    std::string stripped;
    stripped.reserve(source.size());
    for (size_t i = 0; i < source.size();)
    {
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/'))
                ++i;
            if (i + 1 < source.size())
                i += 2;
            stripped.push_back(' ');
        }
        else
        {
            stripped.push_back(source[i++]);
        }
    }

    auto skipWs = [](std::string_view s, size_t& i)
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
    };

    size_t pos = 0;
    while (pos < stripped.size())
    {
        size_t lineEnd = stripped.find('\n', pos);
        if (lineEnd == std::string::npos)
            lineEnd = stripped.size();
        std::string_view line(stripped.data() + pos, lineEnd - pos);
        pos = lineEnd + 1;

        size_t i = 0;
        skipWs(line, i);
        if (i + 1 >= line.size() || line[i] != '/' || line[i + 1] != '/')
            continue;
        i += 2;
        skipWs(line, i);

        // Accept either "[permutation] NAME=..." (constant / macro axis) or
        // "[permutation type] NAME=..." (generic type-parameter axis).
        constexpr std::string_view markerC = "[permutation]";
        constexpr std::string_view markerT = "[permutation type]";

        PermutationDefine::Kind kind = PermutationDefine::Kind::Constant;
        if (line.size() - i >= markerT.size() && line.substr(i, markerT.size()) == markerT)
        {
            kind = PermutationDefine::Kind::Type;
            i += markerT.size();
        }
        else if (line.size() - i >= markerC.size() && line.substr(i, markerC.size()) == markerC)
        {
            i += markerC.size();
        }
        else
        {
            continue;
        }
        skipWs(line, i);

        if (i >= line.size() || !isIdentStart(line[i]))
            continue;
        size_t nameStart = i;
        while (i < line.size() && isIdentCont(line[i]))
            ++i;
        std::string name(line.substr(nameStart, i - nameStart));

        skipWs(line, i);
        if (i >= line.size() || line[i] != '=')
            continue;
        ++i;
        skipWs(line, i);
        if (i >= line.size() || line[i] != '{')
            continue;
        ++i;

        const bool allowAngleNesting = (kind == PermutationDefine::Kind::Type);
        auto       close             = findDirectiveValueListEnd(line, i, allowAngleNesting);
        if (!close.has_value())
            continue;

        auto values = detail::parsePermutationValueList(line.substr(i, *close - i), allowAngleNesting);
        if (!values.has_value() || values->empty())
            continue;

        PermutationDefine def;
        def.kind   = kind;
        def.name   = std::move(name);
        def.values = std::move(*values);
        out.push_back(std::move(def));
    }
    return out;
}

std::vector<PermutationDefine> PermutationParser::parseFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse(s);
}

bool sourceHasNoReflection(std::string_view source)
{
    // Line-based scan identical in spirit to PermutationParser::parse:
    // strip block comments first, then look for "// [noreflection]" with any
    // amount of surrounding whitespace.
    std::string stripped;
    stripped.reserve(source.size());
    for (size_t i = 0; i < source.size();)
    {
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/'))
                ++i;
            if (i + 1 < source.size())
                i += 2;
            stripped.push_back(' ');
        }
        else
        {
            stripped.push_back(source[i++]);
        }
    }

    auto skipWs = [](std::string_view s, size_t& i)
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
    };

    size_t pos = 0;
    while (pos < stripped.size())
    {
        size_t lineEnd = stripped.find('\n', pos);
        if (lineEnd == std::string::npos)
            lineEnd = stripped.size();
        std::string_view line(stripped.data() + pos, lineEnd - pos);
        pos = lineEnd + 1;

        size_t i = 0;
        skipWs(line, i);
        if (i + 1 >= line.size() || line[i] != '/' || line[i + 1] != '/')
            continue;
        i += 2;
        skipWs(line, i);
        constexpr std::string_view marker = "[noreflection]";
        if (line.size() - i < marker.size())
            continue;
        if (line.substr(i, marker.size()) == marker)
            return true;
    }
    return false;
}

bool fileHasNoReflection(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return sourceHasNoReflection(s);
}

std::vector<Permutation> PermutationExpander::expand(const std::vector<PermutationDefine>& defs)
{
    std::vector<Permutation> out;
    out.emplace_back();
    for (const auto& d : defs)
    {
        if (d.values.empty())
            continue;
        std::vector<Permutation> next;
        next.reserve(out.size() * d.values.size());
        for (const auto& base : out)
        {
            for (const auto& v : d.values)
            {
                Permutation p = base;
                if (d.kind == PermutationDefine::Kind::Type)
                    p.typeArgs.push_back({d.name, v});
                else
                    p.constants.push_back({d.name, v});
                next.push_back(std::move(p));
            }
        }
        out = std::move(next);
    }
    for (auto& p : out)
    {
        auto byName = [](const ShaderConstant& a, const ShaderConstant& b) { return a.name < b.name; };
        std::ranges::sort(p.constants, byName);
        std::ranges::sort(p.typeArgs, byName);
    }
    return out;
}

std::vector<PermutationDefine> mergePermutationDefines(const std::vector<PermutationDefine>& fileDefines,
                                                       const std::vector<PermutationDefine>& cliOverride)
{
    std::vector<PermutationDefine>  out;
    std::unordered_set<std::string> overridden;
    for (const auto& d : cliOverride)
        overridden.insert(d.name);
    for (const auto& d : fileDefines)
    {
        if (!overridden.contains(d.name))
            out.push_back(d);
    }
    for (const auto& d : cliOverride)
        out.push_back(d);
    return out;
}

namespace detail
{

uint64_t hashFileContents(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return 0;
    uint64_t h = kFnvOffset;
    char     buf[4096];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0)
    {
        fnv1aMix(h, buf, static_cast<size_t>(f.gcount()));
    }
    return h;
}

uint64_t hashCompileOptions(const CompileOptions& o)
{
    uint64_t h = kFnvOffset;
    fnv1aMixPod(h, o.target);
    fnv1aMixStr(h, o.profile);
    fnv1aMixStr(h, o.entryPoint);
    fnv1aMixPod(h, o.optimization);
    fnv1aMixPod(h, o.matrixLayout);
    fnv1aMixPod(h, o.fpMode);
    fnv1aMixPod(h, o.debugInfo);
    fnv1aMixPod(h, o.warningsAsErrors);
    fnv1aMixPod(h, o.glslScalarLayout);
    fnv1aMixPod(h, o.emitSpirvDirectly);
    fnv1aMixPod(h, o.dumpIntermediates);
    fnv1aMixPod(h, o.emitReflection);
    fnv1aMixPod(h, o.vulkanVersion);
    fnv1aMixStr(h, o.languageVersion);
    for (const auto& p : o.includePaths)
        fnv1aMixStr(h, p.string());

    // Sort global defines so the hash is independent of user-supplied order.
    std::vector<std::pair<std::string, std::string>> defs;
    defs.reserve(o.defines.size());
    for (const auto& d : o.defines)
        defs.emplace_back(d.name, d.value);
    std::ranges::sort(defs);
    for (const auto& d : defs)
    {
        fnv1aMixStr(h, d.first);
        fnv1aMixStr(h, d.second);
    }
    for (const auto& bs : o.vulkanBindShifts)
    {
        fnv1aMixPod(h, bs.kind);
        fnv1aMixPod(h, bs.space);
        fnv1aMixPod(h, bs.shift);
    }
    return h;
}

} // namespace detail

} // namespace slangmake
