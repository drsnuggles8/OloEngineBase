// =============================================================================
// CommandLifecycleCheck.h
//
// Suite-wide command-lifecycle guard (issue #1335).
//
// The test binary runs with CommandLifecycle validation ON, so every replay in
// every test — the synthetic buckets of the plumbing tests and the real frames
// of every renderer and evidence test — freezes its packets, digests them, and
// re-checks them before and after the replay. This listener fails any test
// during which a lifecycle violation was raised outside a
// CommandLifecycle::ScopedExpectedViolations. A negative control scopes exactly
// the operation it expects to be refused; anything else it triggers still
// fails it.
//
// That turns the whole suite into the evidence that real scene submission,
// bone-offset remapping, batching and capture prepare their packets BEFORE the
// first replay and never write them after it: a production path that did would
// fail whichever test drove it, by name, instead of producing a racy frame.
// =============================================================================
#pragma once

namespace OloEngine::Tests::CommandLifecycleCheck
{
    // Turns validation on and installs the listener. Call once from main()
    // after InitGoogleTest. Idempotent.
    void RegisterListener();
} // namespace OloEngine::Tests::CommandLifecycleCheck
