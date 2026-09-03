#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Splat/GaussianSplatCloud.h"

#include "OloEngine/Debug/Instrumentor.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace OloEngine::GaussianSplat
{
    namespace
    {
        // PLY scalar types, by the names the spec and every exporter use.
        // Only their SIZE matters here: the reader locates the properties it
        // wants by name and strides past everything else, so an exporter that
        // writes extra `uchar` classification columns costs nothing.
        [[nodiscard]] sizet PlyTypeSize(std::string_view type)
        {
            if (type == "float" || type == "float32" || type == "int" || type == "int32" || type == "uint" ||
                type == "uint32")
                return 4;
            if (type == "double" || type == "float64")
                return 8;
            if (type == "short" || type == "int16" || type == "ushort" || type == "uint16")
                return 2;
            if (type == "char" || type == "int8" || type == "uchar" || type == "uint8")
                return 1;
            return 0; // includes `list`, which this reader does not support
        }

        // Largest value the IEEE half format can represent. Lives here rather
        // than inside PackCovariance because a constexpr local is odr-used by
        // std::clamp's by-reference parameters and cannot be implicitly
        // captured by a capture-less lambda.
        constexpr f32 kHalfMax = 65504.0f;

        struct PlyProperty
        {
            std::string Name;
            std::string Type;
            sizet Offset = 0;
        };

        // Reads one scalar out of a binary record. Everything the importer wants
        // is authored as `float`, but reading a `double` correctly costs three
        // lines and turns a corrupt cloud into a correct one for the exporters
        // that do write doubles.
        [[nodiscard]] f32 ReadScalar(const u8* record, const PlyProperty& prop)
        {
            const u8* src = record + prop.Offset;
            if (prop.Type == "double" || prop.Type == "float64")
            {
                f64 v = 0.0;
                std::memcpy(&v, src, sizeof(v));
                return static_cast<f32>(v);
            }
            if (prop.Type == "uchar" || prop.Type == "uint8")
                return static_cast<f32>(*src);
            if (prop.Type == "char" || prop.Type == "int8")
                return static_cast<f32>(static_cast<i8>(*src));
            f32 v = 0.0f;
            std::memcpy(&v, src, sizeof(v));
            return v;
        }

        [[nodiscard]] std::string_view TrimTrailing(std::string_view line)
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.remove_suffix(1);
            return line;
        }

        // Splits on runs of spaces and tabs.
        [[nodiscard]] std::vector<std::string_view> Tokenize(std::string_view line)
        {
            std::vector<std::string_view> out;
            sizet i = 0;
            while (i < line.size())
            {
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                    ++i;
                const sizet start = i;
                while (i < line.size() && line[i] != ' ' && line[i] != '\t')
                    ++i;
                if (i > start)
                    out.push_back(line.substr(start, i - start));
            }
            return out;
        }

        // std::from_chars is the only float parse here that is locale-immune,
        // which matters because a comma-decimal locale makes strtof truncate
        // every coordinate silently.
        [[nodiscard]] bool ParseFloat(std::string_view token, f32& out)
        {
            const char* first = token.data();
            const char* last = token.data() + token.size();
            const auto res = std::from_chars(first, last, out);
            return res.ec == std::errc{} && res.ptr == last && std::isfinite(out);
        }
    } // namespace

    auto SigmoidOpacity(f32 logit) -> f32
    {
        // Clamped before exp so a corrupt property cannot produce inf or NaN.
        // The clamp is far outside what a trainer writes: sigmoid is saturated
        // to within 1e-13 at |x| = 30.
        const f32 x = std::clamp(logit, -30.0f, 30.0f);
        return 1.0f / (1.0f + std::exp(-x));
    }

    auto ShDcToColor(const glm::vec3& shDc) -> glm::vec3
    {
        return glm::clamp(glm::vec3(0.5f) + kShC0 * shDc, glm::vec3(0.0f), glm::vec3(1.0f));
    }

    auto CovarianceFromScaleRotation(const glm::vec3& logScale, const glm::vec4& rotationWXYZ) -> std::array<f32, 6>
    {
        // scale_* holds log(sigma). exp() of a large positive value is the one
        // place this turns finite input into inf, so the exponent is clamped:
        // e^20 is 4.8e8 world units, already absurd for any scan.
        const glm::vec3 s = glm::exp(glm::clamp(logScale, glm::vec3(-30.0f), glm::vec3(20.0f)));

        // The PLY stores the quaternion as (w, x, y, z) and does NOT guarantee
        // it is normalised -- the trainer normalises in its own shader, not on
        // write. An all-zero quaternion becomes identity rather than a NaN
        // rotation matrix.
        glm::vec4 q = rotationWXYZ;
        const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        q = (len > 1e-8f) ? q / len : glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);

        const f32 w = q.x;
        const f32 x = q.y;
        const f32 y = q.z;
        const f32 z = q.w;
        const glm::mat3 R{
            glm::vec3(1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + w * z), 2.0f * (x * z - w * y)),
            glm::vec3(2.0f * (x * y - w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + w * x)),
            glm::vec3(2.0f * (x * z + w * y), 2.0f * (y * z - w * x), 1.0f - 2.0f * (x * x + y * y)),
        };

        // M = R * S, Sigma = M * M^T. Writing it that way rather than
        // R S S^T R^T is the same arithmetic and symmetric by construction, so
        // the upper triangle taken below is exact rather than nearly-symmetric.
        const glm::mat3 M{ R[0] * s.x, R[1] * s.y, R[2] * s.z };
        const glm::mat3 sigma = M * glm::transpose(M);
        return { sigma[0][0], sigma[1][0], sigma[2][0], sigma[1][1], sigma[2][1], sigma[2][2] };
    }

    void PackCovariance(const std::array<f32, 6>& sigma, u32& covXXXY, u32& covXZYY, u32& covYZZZ)
    {
        // CLAMPED TO THE HALF RANGE, not merely converted. glm::packHalf2x16
        // maps anything above 65504 to +inf, and an infinite covariance term
        // makes ConservativeSigma infinite, which makes `depth - radius` -inf,
        // which drops the splat from every camera with nothing logged. 65504 is
        // a sigma of about 256 world units -- far past any real splat, so a
        // value that hits the clamp was already nonsense; the clamp only
        // decides whether it is nonsense that still draws.
        const auto clampTerm = [](f32 v)
        { return std::clamp(v, -kHalfMax, kHalfMax); };
        covXXXY = glm::packHalf2x16(glm::vec2(clampTerm(sigma[0]), clampTerm(sigma[1])));
        covXZYY = glm::packHalf2x16(glm::vec2(clampTerm(sigma[2]), clampTerm(sigma[3])));
        covYZZZ = glm::packHalf2x16(glm::vec2(clampTerm(sigma[4]), clampTerm(sigma[5])));
    }

    auto UnpackCovariance(u32 covXXXY, u32 covXZYY, u32 covYZZZ) -> std::array<f32, 6>
    {
        const glm::vec2 a = glm::unpackHalf2x16(covXXXY);
        const glm::vec2 b = glm::unpackHalf2x16(covXZYY);
        const glm::vec2 c = glm::unpackHalf2x16(covYZZZ);
        return { a.x, a.y, b.x, b.y, c.x, c.y };
    }

    auto PackColorOpacity(const glm::vec3& linearRgb, f32 opacity) -> u32
    {
        const glm::vec4 c = glm::clamp(glm::vec4(linearRgb, opacity), glm::vec4(0.0f), glm::vec4(1.0f));
        const auto quantize = [](f32 v)
        { return static_cast<u32>(v * 255.0f + 0.5f) & 0xFFu; };
        return quantize(c.x) | (quantize(c.y) << 8) | (quantize(c.z) << 16) | (quantize(c.w) << 24);
    }

    auto UnpackColorOpacity(u32 packed) -> glm::vec4
    {
        return glm::vec4(static_cast<f32>(packed & 0xFFu), static_cast<f32>((packed >> 8) & 0xFFu),
                         static_cast<f32>((packed >> 16) & 0xFFu), static_cast<f32>((packed >> 24) & 0xFFu)) /
               255.0f;
    }

    void SplatCloud::Clear()
    {
        m_Splats.clear();
        m_Bounds = BoundingBox{};
        m_MaxRadius = 0.0f;
    }

    void SplatCloud::Build(std::span<const glm::vec3> positions,
                           std::span<const glm::vec3> shDc,
                           std::span<const f32> logitOpacity,
                           std::span<const glm::vec3> logScale,
                           std::span<const glm::vec4> rotationWXYZ)
    {
        OLO_PROFILE_FUNCTION();

        const sizet count = positions.size();

        Clear();

        // The assert names the bug for a debug build; the early return is what
        // makes a Release build safe, because OLO_CORE_ASSERT compiles out
        // entirely outside OLO_DEBUG and the loop below would then read past
        // the end of whichever span was short.
        if (shDc.size() != count || logitOpacity.size() != count || logScale.size() != count ||
            rotationWXYZ.size() != count)
        {
            OLO_CORE_ASSERT(false, "GaussianSplat::SplatCloud::Build: parallel arrays must be the same length");
            return;
        }

        m_Splats.resize(count);

        glm::vec3 lo(std::numeric_limits<f32>::max());
        glm::vec3 hi(std::numeric_limits<f32>::lowest());
        f32 maxRadius = 0.0f;

        for (sizet i = 0; i < count; ++i)
        {
            GpuSplat& out = m_Splats[i];
            out.Position = positions[i];
            out.ColorOpacity = PackColorOpacity(ShDcToColor(shDc[i]), SigmoidOpacity(logitOpacity[i]));

            const std::array<f32, 6> sigma = CovarianceFromScaleRotation(logScale[i], rotationWXYZ[i]);
            PackCovariance(sigma, out.CovXXXY, out.CovXZYY, out.CovYZZZ);

            lo = glm::min(lo, out.Position);
            hi = glm::max(hi, out.Position);

            // 3 sigma along the longest axis. The trace bounds the largest
            // eigenvalue from above, so this radius is conservative -- which is
            // what a guard band wants.
            const f32 trace = sigma[0] + sigma[3] + sigma[5];
            maxRadius = std::max(maxRadius, 3.0f * std::sqrt(std::max(trace, 0.0f)));
        }

        if (count > 0)
            m_Bounds = BoundingBox(lo, hi);
        m_MaxRadius = maxRadius;
    }

    auto SplatCloud::LoadPly(const std::filesystem::path& path) -> LoadResult
    {
        OLO_PROFILE_FUNCTION();

        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            LoadResult bad;
            bad.Error = "GaussianSplat: cannot open '" + path.string() + "'";
            return bad;
        }

        const std::vector<u8> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return LoadPlyFromMemory(bytes, path.string());
    }

    auto SplatCloud::LoadPlyFromMemory(std::span<const u8> bytes, std::string_view sourceName) -> LoadResult
    {
        OLO_PROFILE_FUNCTION();

        Clear();
        const auto fail = [&](const std::string& what)
        {
            LoadResult bad;
            bad.Error = "GaussianSplat: '" + std::string(sourceName) + "': " + what;
            return bad;
        };

        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!text.starts_with("ply"))
            return fail("not a PLY (missing the 'ply' magic)");

        // ---- header --------------------------------------------------------
        bool binaryLittleEndian = false;
        bool ascii = false;
        u32 vertexCount = 0;
        bool sawVertexElement = false;
        bool afterVertexElement = false;
        bool sawEndHeader = false;
        std::vector<PlyProperty> props;
        sizet stride = 0;
        sizet cursor = 0;

        while (cursor < text.size())
        {
            const sizet newline = text.find('\n', cursor);
            if (newline == std::string_view::npos)
                return fail("header has no 'end_header' line");
            const std::string_view line = TrimTrailing(text.substr(cursor, newline - cursor));
            cursor = newline + 1;

            const std::vector<std::string_view> tok = Tokenize(line);
            if (tok.empty())
                continue;

            if (tok[0] == "end_header")
            {
                sawEndHeader = true;
                break;
            }

            if (tok[0] == "format")
            {
                if (tok.size() < 2)
                    return fail("malformed 'format' line");
                if (tok[1] == "binary_little_endian")
                    binaryLittleEndian = true;
                else if (tok[1] == "ascii")
                    ascii = true;
                else
                    return fail("unsupported PLY format '" + std::string(tok[1]) +
                                "' (this reader handles binary_little_endian and ascii)");
            }
            else if (tok[0] == "element")
            {
                if (tok.size() < 3)
                    return fail("malformed 'element' line");
                if (tok[1] == "vertex")
                {
                    if (sawVertexElement)
                        return fail("two 'vertex' elements");
                    sawVertexElement = true;
                    u32 parsed = 0;
                    const auto res = std::from_chars(tok[2].data(), tok[2].data() + tok[2].size(), parsed);
                    if (res.ec != std::errc{})
                        return fail("'element vertex' count is not a number");
                    vertexCount = parsed;
                }
                else if (sawVertexElement)
                {
                    // Properties from here on belong to a later element.
                    afterVertexElement = true;
                }
                else
                {
                    // An element BEFORE vertex shifts the binary payload by an
                    // amount this reader does not track. Refuse rather than
                    // read the wrong bytes and produce a plausible wrong cloud.
                    return fail("element '" + std::string(tok[1]) +
                                "' precedes 'vertex'; this reader requires vertex to be the first element");
                }
            }
            else if (tok[0] == "property" && sawVertexElement && !afterVertexElement)
            {
                if (tok.size() < 3)
                    return fail("malformed 'property' line");
                if (tok[1] == "list")
                    return fail("'property list' inside the vertex element is not supported");
                const sizet size = PlyTypeSize(tok[1]);
                if (size == 0)
                    return fail("unknown property type '" + std::string(tok[1]) + "'");
                props.push_back({ std::string(tok[2]), std::string(tok[1]), stride });
                stride += size;
            }
        }

        if (!sawEndHeader)
            return fail("header has no 'end_header' line");
        if (!binaryLittleEndian && !ascii)
            return fail("header has no 'format' line");
        if (!sawVertexElement)
            return fail("header has no 'vertex' element");

        // ---- required properties -------------------------------------------
        std::unordered_map<std::string_view, const PlyProperty*> byName;
        byName.reserve(props.size());
        for (const PlyProperty& prop : props)
            byName.emplace(prop.Name, &prop);

        std::vector<std::string> missing;
        const auto need = [&](std::string_view name) -> const PlyProperty*
        {
            const auto it = byName.find(name);
            if (it == byName.end())
            {
                missing.emplace_back(name);
                return nullptr;
            }
            return it->second;
        };

        const PlyProperty* px = need("x");
        const PlyProperty* py = need("y");
        const PlyProperty* pz = need("z");
        const PlyProperty* pOpacity = need("opacity");
        const std::array<const PlyProperty*, 3> pDc{ need("f_dc_0"), need("f_dc_1"), need("f_dc_2") };
        const std::array<const PlyProperty*, 3> pScale{ need("scale_0"), need("scale_1"), need("scale_2") };
        const std::array<const PlyProperty*, 4> pRot{ need("rot_0"), need("rot_1"), need("rot_2"), need("rot_3") };

        if (!missing.empty())
        {
            std::string list;
            for (const std::string& name : missing)
                list += (list.empty() ? "" : ", ") + name;
            return fail("vertex element is missing required propert" +
                        std::string(missing.size() == 1 ? "y " : "ies ") + list);
        }

        // ---- payload --------------------------------------------------------
        // THE COUNT IS CHECKED AGAINST THE FILE BEFORE ANYTHING IS ALLOCATED.
        // `element vertex` is untrusted input: a 250-byte file that declares
        // 4294967295 vertices would otherwise reserve ~240 GB here and die on
        // std::bad_alloc, three lines before the truncation check that exists
        // to report exactly that. An importer that throws where it promised to
        // return an error is not degrading gracefully.
        //
        // The bound differs per format because the minimum bytes per record
        // does: `stride` exactly for binary, and for ASCII at least one
        // character plus a separator per column.
        {
            const sizet remaining = bytes.size() - cursor;
            // ASCII needs at least one character and one separator per column,
            // less the separator the last column does not have. `props` cannot
            // be empty here (the required-property check above would have
            // failed), but the guard keeps the subtraction from wrapping if
            // that ever stops being true.
            const sizet asciiMinimum = props.empty() ? 1 : (2 * props.size() - 1);
            const sizet minimumBytesPerVertex = binaryLittleEndian ? stride : asciiMinimum;
            const sizet maximumPossible = (minimumBytesPerVertex > 0) ? (remaining / minimumBytesPerVertex) : 0;
            if (static_cast<sizet>(vertexCount) > maximumPossible)
                return fail("truncated: header promises " + std::to_string(vertexCount) +
                            " vertices, but the remaining " + std::to_string(remaining) +
                            " bytes can hold at most " + std::to_string(maximumPossible));
        }

        std::vector<glm::vec3> positions(vertexCount);
        std::vector<glm::vec3> shDc(vertexCount);
        std::vector<f32> opacity(vertexCount);
        std::vector<glm::vec3> logScale(vertexCount);
        std::vector<glm::vec4> rotation(vertexCount);

        if (binaryLittleEndian)
        {
            const sizet payload = static_cast<sizet>(vertexCount) * stride;
            if (bytes.size() - cursor < payload)
                return fail("truncated: header promises " + std::to_string(vertexCount) + " vertices (" +
                            std::to_string(payload) + " bytes) but only " + std::to_string(bytes.size() - cursor) +
                            " remain");

            const u8* base = bytes.data() + cursor;
            for (u32 i = 0; i < vertexCount; ++i)
            {
                const u8* rec = base + static_cast<sizet>(i) * stride;
                positions[i] = { ReadScalar(rec, *px), ReadScalar(rec, *py), ReadScalar(rec, *pz) };
                shDc[i] = { ReadScalar(rec, *pDc[0]), ReadScalar(rec, *pDc[1]), ReadScalar(rec, *pDc[2]) };
                opacity[i] = ReadScalar(rec, *pOpacity);
                logScale[i] = { ReadScalar(rec, *pScale[0]), ReadScalar(rec, *pScale[1]),
                                ReadScalar(rec, *pScale[2]) };
                rotation[i] = { ReadScalar(rec, *pRot[0]), ReadScalar(rec, *pRot[1]), ReadScalar(rec, *pRot[2]),
                                ReadScalar(rec, *pRot[3]) };
            }
        }
        else
        {
            // ASCII: one whitespace-separated record per line, in header order.
            std::vector<f32> values(props.size());
            const auto columnOf = [&props](const PlyProperty* prop)
            {
                return static_cast<sizet>(prop - props.data());
            };
            for (u32 i = 0; i < vertexCount; ++i)
            {
                if (cursor >= text.size())
                    return fail("truncated: ran out of lines at vertex " + std::to_string(i) + " of " +
                                std::to_string(vertexCount));
                const sizet newline = text.find('\n', cursor);
                const sizet end = (newline == std::string_view::npos) ? text.size() : newline;
                const std::string_view line = TrimTrailing(text.substr(cursor, end - cursor));
                cursor = (newline == std::string_view::npos) ? text.size() : newline + 1;

                const std::vector<std::string_view> tok = Tokenize(line);
                if (tok.size() < props.size())
                    return fail("vertex " + std::to_string(i) + " has " + std::to_string(tok.size()) +
                                " values, expected " + std::to_string(props.size()));
                for (sizet c = 0; c < props.size(); ++c)
                {
                    if (!ParseFloat(tok[c], values[c]))
                        return fail("vertex " + std::to_string(i) + " column " + std::to_string(c) +
                                    " is not a finite number ('" + std::string(tok[c]) + "')");
                }

                positions[i] = { values[columnOf(px)], values[columnOf(py)], values[columnOf(pz)] };
                shDc[i] = { values[columnOf(pDc[0])], values[columnOf(pDc[1])], values[columnOf(pDc[2])] };
                opacity[i] = values[columnOf(pOpacity)];
                logScale[i] = { values[columnOf(pScale[0])], values[columnOf(pScale[1])],
                                values[columnOf(pScale[2])] };
                rotation[i] = { values[columnOf(pRot[0])], values[columnOf(pRot[1])], values[columnOf(pRot[2])],
                                values[columnOf(pRot[3])] };
            }
        }

        // Every float here came off disk: cpp-coding-quality.md 2b.
        const auto finite3 = [](const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        };
        for (u32 i = 0; i < vertexCount; ++i)
        {
            if (!finite3(positions[i]) || !finite3(shDc[i]) || !finite3(logScale[i]) || !std::isfinite(opacity[i]) ||
                !finite3(glm::vec3(rotation[i])) || !std::isfinite(rotation[i].w))
                return fail("vertex " + std::to_string(i) + " contains a non-finite value");
        }

        Build(positions, shDc, opacity, logScale, rotation);

        sizet keptBytes = 0;
        const auto countKept = [&](const PlyProperty* prop)
        { keptBytes += PlyTypeSize(prop->Type); };
        countKept(px);
        countKept(py);
        countKept(pz);
        countKept(pOpacity);
        for (const PlyProperty* prop : pDc)
            countKept(prop);
        for (const PlyProperty* prop : pScale)
            countKept(prop);
        for (const PlyProperty* prop : pRot)
            countKept(prop);

        LoadResult result;
        result.Ok = true;
        result.SplatsRead = vertexCount;
        result.DiscardedSourceBytes = (stride - keptBytes) * static_cast<sizet>(vertexCount);
        return result;
    }
} // namespace OloEngine::GaussianSplat
