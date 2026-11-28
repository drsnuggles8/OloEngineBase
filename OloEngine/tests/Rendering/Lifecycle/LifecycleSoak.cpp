#include "OloEnginePCH.h"
#include "LifecycleSoak.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>

namespace OloEngine::Tests::Lifecycle
{
    namespace
    {
        constexpr std::array<char, 8> kFrameMagic{ 'O', 'L', 'O', 'S', 'O', 'A', 'K', '1' };

        [[nodiscard]] std::string Percent(f64 fraction)
        {
            std::ostringstream text;
            text << std::fixed << std::setprecision(3) << fraction * 100.0 << " %";
            return text.str();
        }

        [[nodiscard]] std::string Mib(f64 bytes)
        {
            std::ostringstream text;
            text << std::fixed << std::setprecision(2) << bytes / (1024.0 * 1024.0) << " MiB";
            return text.str();
        }

        void WriteString(std::ostream& out, std::string_view text)
        {
            out << '"';
            for (const char c : text)
            {
                switch (c)
                {
                    case '"':
                        out << "\\\"";
                        break;
                    case '\\':
                        out << "\\\\";
                        break;
                    case '\n':
                        out << "\\n";
                        break;
                    case '\r':
                        out << "\\r";
                        break;
                    case '\t':
                        out << "\\t";
                        break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20u)
                        {
                            out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                                << static_cast<int>(static_cast<unsigned char>(c)) << std::dec << std::setfill(' ');
                        }
                        else
                        {
                            out << c;
                        }
                        break;
                }
            }
            out << '"';
        }

        // JSON has no infinity; a shape mismatch writes null for its shift.
        void WriteNumber(std::ostream& out, f64 value)
        {
            if (std::isfinite(value))
                out << value;
            else
                out << "null";
        }

        void WriteDiff(std::ostream& out, const FrameDiff& diff)
        {
            out << "{\"shapeMismatch\":" << (diff.ShapeMismatch ? "true" : "false") << ",\"maxTileShift\":";
            WriteNumber(out, diff.MaxTileShift);
            out << ",\"differingFraction\":";
            WriteNumber(out, diff.DifferingFraction);
            out << ",\"maxDelta\":" << diff.MaxDelta << '}';
        }

        void WriteTimes(std::ostream& out, const FrameTimeTailStats& stats)
        {
            out << "{\"samples\":" << stats.SampleCount << ",\"minMs\":" << stats.MinMs << ",\"meanMs\":" << stats.MeanMs
                << ",\"p50Ms\":" << stats.P50Ms << ",\"p95Ms\":" << stats.P95Ms << ",\"p99Ms\":" << stats.P99Ms
                << ",\"maxMs\":" << stats.MaxMs << '}';
        }

        void WriteGrowth(std::ostream& out, const GrowthVerdict& growth)
        {
            out << "{\"first\":" << growth.First << ",\"last\":" << growth.Last << ",\"peak\":" << growth.Peak
                << ",\"growth\":" << growth.Growth << ",\"slopePerCycle\":" << growth.SlopePerCycle
                << ",\"toleranceBytes\":" << growth.ToleranceBytes
                << ",\"bounded\":" << (growth.Bounded ? "true" : "false") << '}';
        }

        template<typename T, typename F>
        void WriteArray(std::ostream& out, const std::vector<T>& items, F&& writeOne)
        {
            out << '[';
            for (sizet i = 0; i < items.size(); ++i)
            {
                if (i != 0)
                    out << ", ";
                writeOne(items[i]);
            }
            out << ']';
        }
    } // namespace

    std::string FrameDiff::Describe() const
    {
        if (ShapeMismatch)
            return "frames of different sizes";
        std::ostringstream text;
        text << "tile shift " << std::fixed << std::setprecision(2) << MaxTileShift << "/255, "
             << Percent(DifferingFraction) << " of pixels over " << kPixelThreshold << ", max delta " << MaxDelta;
        return text.str();
    }

    FrameDiff CompareFrames(const Frame& a, const Frame& b)
    {
        FrameDiff diff;
        const sizet expected = static_cast<sizet>(a.Width) * a.Height * 4u;
        if (a.Width != b.Width || a.Height != b.Height || a.Width == 0u || a.Height == 0u ||
            a.Rgba.size() != expected || b.Rgba.size() != expected)
        {
            diff.ShapeMismatch = true;
            diff.MaxTileShift = std::numeric_limits<f64>::infinity();
            diff.DifferingFraction = 1.0;
            diff.MaxDelta = 255u;
            return diff;
        }

        u64 differing = 0;
        for (sizet i = 0; i < expected; i += 4u)
        {
            u32 pixelMax = 0;
            for (sizet c = 0; c < 4u; ++c)
            {
                const i32 delta = static_cast<i32>(a.Rgba[i + c]) - static_cast<i32>(b.Rgba[i + c]);
                pixelMax = std::max(pixelMax, static_cast<u32>(std::abs(delta)));
            }
            diff.MaxDelta = std::max(diff.MaxDelta, pixelMax);
            if (pixelMax > kPixelThreshold)
                ++differing;
        }
        diff.DifferingFraction = static_cast<f64>(differing) / (static_cast<f64>(a.Width) * a.Height);

        // Tiles of kTileSize a side; the last row and column absorb the
        // remainder rather than forming a sliver whose mean is one pixel.
        const u32 tilesX = std::max(1u, a.Width / kTileSize);
        const u32 tilesY = std::max(1u, a.Height / kTileSize);
        for (u32 ty = 0; ty < tilesY; ++ty)
        {
            const u32 y0 = ty * kTileSize;
            const u32 y1 = (ty + 1u == tilesY) ? a.Height : y0 + kTileSize;
            for (u32 tx = 0; tx < tilesX; ++tx)
            {
                const u32 x0 = tx * kTileSize;
                const u32 x1 = (tx + 1u == tilesX) ? a.Width : x0 + kTileSize;
                std::array<i64, 4> sum{};
                for (u32 y = y0; y < y1; ++y)
                {
                    for (u32 x = x0; x < x1; ++x)
                    {
                        const sizet i = (static_cast<sizet>(y) * a.Width + x) * 4u;
                        for (sizet c = 0; c < 4u; ++c)
                            sum[c] += static_cast<i64>(a.Rgba[i + c]) - static_cast<i64>(b.Rgba[i + c]);
                    }
                }
                const f64 count = static_cast<f64>(x1 - x0) * static_cast<f64>(y1 - y0);
                for (const i64 s : sum)
                    diff.MaxTileShift = std::max(diff.MaxTileShift, std::abs(static_cast<f64>(s)) / count);
            }
        }
        return diff;
    }

    std::string FrameVerdict::Describe() const
    {
        return std::string(Held ? "held" : "FAILED") + ": " + Diff.Describe() + " (control floor: " + Floor.Describe() +
               ")";
    }

    FrameVerdict JudgeAgainstControl(const FrameDiff& diff, const FrameDiff& floor)
    {
        FrameVerdict verdict;
        verdict.Diff = diff;
        verdict.Floor = floor;
        // A control that could not be measured is not a tolerance.
        if (diff.ShapeMismatch || floor.ShapeMismatch || !std::isfinite(floor.MaxTileShift) ||
            !std::isfinite(floor.DifferingFraction))
        {
            verdict.Held = false;
            return verdict;
        }
        const f64 tileLimit = std::max(kMinTileShift, floor.MaxTileShift * kFloorScale);
        const f64 fractionLimit = std::max(kMinDifferingFraction, floor.DifferingFraction * kFloorScale);
        verdict.Held = diff.MaxTileShift <= tileLimit && diff.DifferingFraction <= fractionLimit;
        return verdict;
    }

    std::string GrowthVerdict::Describe(std::string_view what) const
    {
        std::ostringstream text;
        text << what << ": " << Mib(static_cast<f64>(First)) << " -> " << Mib(static_cast<f64>(Last)) << " (growth "
             << Mib(static_cast<f64>(Growth)) << ", peak " << Mib(static_cast<f64>(Peak)) << ", slope "
             << Mib(SlopePerCycle) << "/cycle, tolerance " << Mib(static_cast<f64>(ToleranceBytes)) << ") "
             << (Bounded ? "bounded" : "GROWING");
        return text.str();
    }

    GrowthVerdict JudgeGrowth(std::span<const u64> samples, u64 toleranceBytes)
    {
        GrowthVerdict verdict;
        verdict.ToleranceBytes = toleranceBytes;
        if (samples.empty())
            return verdict;
        verdict.First = samples.front();
        verdict.Last = samples.back();
        verdict.Peak = *std::ranges::max_element(samples);
        verdict.Growth = static_cast<i64>(verdict.Last) - static_cast<i64>(verdict.First);
        verdict.Bounded = verdict.Growth <= static_cast<i64>(toleranceBytes);

        // Least-squares slope over the sample index.
        if (samples.size() >= 2u)
        {
            const f64 n = static_cast<f64>(samples.size());
            const f64 meanX = (n - 1.0) / 2.0;
            f64 meanY = 0.0;
            for (const u64 s : samples)
                meanY += static_cast<f64>(s);
            meanY /= n;
            f64 numerator = 0.0;
            f64 denominator = 0.0;
            for (sizet i = 0; i < samples.size(); ++i)
            {
                const f64 dx = static_cast<f64>(i) - meanX;
                numerator += dx * (static_cast<f64>(samples[i]) - meanY);
                denominator += dx * dx;
            }
            verdict.SlopePerCycle = denominator > 0.0 ? numerator / denominator : 0.0;
        }
        return verdict;
    }

    void WriteJson(std::ostream& out, const Report& report)
    {
        const auto writeString = [&out](const std::string& s)
        { WriteString(out, s); };

        out << std::setprecision(6);
        out << "{\n  \"schema\": \"olo-lifecycle-soak 1\",\n  \"leg\": ";
        WriteString(out, report.Leg);
        out << ",\n  \"backend\": ";
        WriteString(out, report.Backend);
        out << ",\n  \"build\": ";
        WriteString(out, report.Build);
        out << ",\n  \"seed\": " << report.Seed << ",\n  \"warmUpCycles\": " << report.WarmUpCycles
            << ",\n  \"cycles\": " << report.Cycles << ",\n  \"completedCycles\": " << report.CompletedCycles
            << ",\n  \"operations\": ";
        WriteArray(out, report.Operations, writeString);
        out << ",\n  \"skipped\": ";
        WriteArray(out, report.Skipped, writeString);
        out << ",\n  \"executed\": {";
        for (sizet i = 0; i < report.Executed.size(); ++i)
        {
            out << (i == 0 ? "" : ", ");
            WriteString(out, report.Executed[i].first);
            out << ": " << report.Executed[i].second;
        }
        out << "},\n  \"validationDiagnostics\": ";
        if (report.ValidationDiagnostics)
            out << *report.ValidationDiagnostics;
        else
            out << "null";
        // frameTimes covers the trailing window of frameTimesWindow frames.
        out << ",\n  \"totalFrames\": " << report.TotalFrames
            << ",\n  \"frameTimesWindow\": " << report.AllFrameTimes.SampleCount << ",\n  \"frameTimes\": ";
        WriteTimes(out, report.AllFrameTimes);

        const auto writeOptionalGrowth = [&out](const char* key, const std::optional<GrowthVerdict>& growth)
        {
            out << ",\n  \"" << key << "\": ";
            if (growth)
                WriteGrowth(out, *growth);
            else
                out << "null";
        };
        const auto writeOptionalDiff = [&out](const char* key, const std::optional<FrameDiff>& diff)
        {
            out << ",\n  \"" << key << "\": ";
            if (diff)
                WriteDiff(out, *diff);
            else
                out << "null";
        };
        writeOptionalGrowth("gpuGrowth", report.GpuGrowth);
        writeOptionalGrowth("processGrowth", report.ProcessGrowth);
        writeOptionalDiff("afterRestart", report.AfterRestart);
        writeOptionalDiff("freshProcessControl", report.FreshProcessControl);

        out << ",\n  \"cycleRecords\": [";
        for (sizet i = 0; i < report.Records.size(); ++i)
        {
            const CycleRecord& r = report.Records[i];
            out << (i == 0 ? "\n    " : ",\n    ") << "{\"cycle\":" << r.Cycle
                << ",\"warmUp\":" << (r.WarmUp ? "true" : "false") << ",\"frameIndexAtReturn\":" << r.FrameIndexAtReturn
                << ",\"gpuResidentBytes\":" << r.GpuResidentBytes << ",\"gpuRetiringBytes\":" << r.GpuRetiringBytes
                << ",\"processResidentBytes\":" << r.ProcessResidentBytes << ",\"returnToA\":";
            WriteDiff(out, r.ReturnToA);
            out << ",\"frameTimes\":";
            WriteTimes(out, r.FrameTimes);
            out << '}';
        }
        out << "\n  ]\n}\n";
    }

    namespace
    {
        // The writer and the reader accept the same frames: a side of 1 to
        // kMaxFrameSide pixels.
        [[nodiscard]] bool SidesInRange(u32 width, u32 height)
        {
            return width != 0u && height != 0u && width <= kMaxFrameSide && height <= kMaxFrameSide;
        }
    } // namespace

    bool SaveFrame(const std::string& path, const Frame& frame)
    {
        if (!SidesInRange(frame.Width, frame.Height) ||
            frame.Rgba.size() != static_cast<sizet>(frame.Width) * frame.Height * 4u)
            return false;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(kFrameMagic.data(), kFrameMagic.size());
        out.write(reinterpret_cast<const char*>(&frame.Width), sizeof(frame.Width));
        out.write(reinterpret_cast<const char*>(&frame.Height), sizeof(frame.Height));
        out.write(reinterpret_cast<const char*>(frame.Rgba.data()), static_cast<std::streamsize>(frame.Rgba.size()));
        // A write the stream buffered can still fail when it is flushed.
        out.close();
        return !out.fail();
    }

    std::optional<Frame> LoadFrame(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return std::nullopt;
        std::array<char, 8> magic{};
        in.read(magic.data(), magic.size());
        if (!in || magic != kFrameMagic)
            return std::nullopt;
        Frame frame;
        in.read(reinterpret_cast<char*>(&frame.Width), sizeof(frame.Width));
        in.read(reinterpret_cast<char*>(&frame.Height), sizeof(frame.Height));
        if (!in || !SidesInRange(frame.Width, frame.Height))
            return std::nullopt;
        // A header promising more pixels than the file holds is refused before
        // the buffer is allocated (a 16K-square header alone would ask for 1 GiB).
        const sizet payload = static_cast<sizet>(frame.Width) * frame.Height * 4u;
        const std::streamoff headerEnd = in.tellg();
        in.seekg(0, std::ios::end);
        const std::streamoff fileEnd = in.tellg();
        if (headerEnd < 0 || fileEnd < headerEnd || static_cast<u64>(fileEnd - headerEnd) != payload)
            return std::nullopt;
        in.seekg(headerEnd);
        frame.Rgba.resize(payload);
        in.read(reinterpret_cast<char*>(frame.Rgba.data()), static_cast<std::streamsize>(frame.Rgba.size()));
        if (!in)
            return std::nullopt;
        // Nothing may follow the pixels.
        if (in.peek() != std::char_traits<char>::eof())
            return std::nullopt;
        return frame;
    }
} // namespace OloEngine::Tests::Lifecycle
