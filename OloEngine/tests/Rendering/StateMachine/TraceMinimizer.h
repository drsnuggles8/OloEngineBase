#pragma once

// =============================================================================
// Delta-debugging trace minimisation for the renderer state-machine harness
// (issue #1349).
//
// A failing sequence found by the generator is usually long and mostly noise:
// forty operations of which two matter. `MinimizeTrace` runs Zeller's ddmin over
// the operation list with a caller-supplied "does this subsequence still fail?"
// predicate and returns the shortest failing subsequence it found, which is what
// gets committed as a regression.
//
// It is generic over the element type and knows nothing about the renderer, so
// the algorithm is pinned by CPU tests against synthetic predicates whose
// minimal answer is known in advance.
//
// Every candidate is a SUBSEQUENCE of the original trace (order preserved,
// elements dropped). The harness makes that sound by giving every operation a
// total definition: an operation is legal in every state, so every subsequence
// of a valid trace is itself a valid trace. A generator that emitted
// state-dependent operations would make most candidates meaningless.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

namespace OloEngine::Tests::StateMachine
{
    template<typename T>
    struct MinimizeResult
    {
        std::vector<T> Trace;
        // Predicate evaluations spent, including the check of the input.
        u32 Evaluations = 0;
        // The input itself did not fail, so there was nothing to minimise.
        bool InputDidNotFail = false;
        // The search ran to completion: removing any single remaining element
        // makes the failure disappear. False when the budget ran out first, in
        // which case `Trace` still fails but may be reducible further.
        bool OneMinimal = false;
    };

    // `fails(candidate)` must return true when the candidate still reproduces
    // the failure. It is called at most `budget` times. The empty trace is
    // never offered: a failure with no operations is a failure of the initial
    // state, which the harness reports separately.
    template<typename T>
    [[nodiscard]] MinimizeResult<T> MinimizeTrace(std::vector<T> trace, const std::function<bool(const std::vector<T>&)>& fails,
                                                  u32 budget)
    {
        MinimizeResult<T> result;
        const auto evaluate = [&result, &fails, budget](const std::vector<T>& candidate) -> std::optional<bool>
        {
            if (result.Evaluations >= budget)
                return std::nullopt;
            ++result.Evaluations;
            return fails(candidate);
        };

        const std::optional<bool> inputFails = evaluate(trace);
        if (!inputFails.has_value() || !*inputFails)
        {
            result.InputDidNotFail = inputFails.has_value();
            result.Trace = std::move(trace);
            return result;
        }

        // Splits `source` into `parts` contiguous chunks whose sizes differ by
        // at most one, and returns chunk `index` (or everything BUT it).
        const auto slice = [](const std::vector<T>& source, sizet parts, sizet index, bool complement)
        {
            const sizet base = source.size() / parts;
            const sizet extra = source.size() % parts;
            const sizet begin = (index * base) + std::min(index, extra);
            const sizet end = begin + base + (index < extra ? 1u : 0u);
            std::vector<T> out;
            out.reserve(complement ? source.size() - (end - begin) : end - begin);
            for (sizet i = 0; i < source.size(); ++i)
            {
                const bool inside = i >= begin && i < end;
                if (inside != complement)
                    out.push_back(source[i]);
            }
            return out;
        };

        sizet granularity = 2;
        while (trace.size() >= 2)
        {
            granularity = std::min(granularity, trace.size());
            bool reduced = false;

            // Reduce to a single chunk.
            for (sizet i = 0; i < granularity && !reduced; ++i)
            {
                std::vector<T> candidate = slice(trace, granularity, i, false);
                if (candidate.empty() || candidate.size() == trace.size())
                    continue;
                const std::optional<bool> outcome = evaluate(candidate);
                if (!outcome.has_value())
                {
                    result.Trace = std::move(trace);
                    return result;
                }
                if (*outcome)
                {
                    trace = std::move(candidate);
                    granularity = 2;
                    reduced = true;
                }
            }

            // Reduce to a complement. With two chunks the complements are the
            // chunks themselves, already tried.
            for (sizet i = 0; i < granularity && !reduced && granularity > 2; ++i)
            {
                std::vector<T> candidate = slice(trace, granularity, i, true);
                if (candidate.empty())
                    continue;
                const std::optional<bool> outcome = evaluate(candidate);
                if (!outcome.has_value())
                {
                    result.Trace = std::move(trace);
                    return result;
                }
                if (*outcome)
                {
                    trace = std::move(candidate);
                    granularity = std::max<sizet>(granularity - 1u, 2u);
                    reduced = true;
                }
            }

            if (reduced)
                continue;
            if (granularity >= trace.size())
                break; // every single-element removal was tried and none still failed
            granularity = std::min(trace.size(), granularity * 2u);
        }

        // A one-element trace is 1-minimal by definition (the empty trace is
        // never offered); a longer one reached here only by exhausting the
        // single-element complements above.
        result.OneMinimal = true;
        result.Trace = std::move(trace);
        return result;
    }
} // namespace OloEngine::Tests::StateMachine
