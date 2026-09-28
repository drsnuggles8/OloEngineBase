# Vulkan query reads need a submission-generation proof

Associate every query rewrite with the submission that executes its reset and
write. Read it only after that submission completes. Vulkan query availability,
including `VK_QUERY_RESULT_WAIT_BIT`, can expose the previous generation until
the GPU executes a newly recorded `vkCmdResetQueryPool`.

The #1488 live attribution sweep exposed this on the dense IntegratedRenderer
scene: all 15 Release/Vulkan Forward whole-frame samples were `outOfOrder`, while
individual pass pairs reported plausible positive durations. The light menu did
not expose the failure. Positive durations therefore did not establish validity;
the entire affected Vulkan timing capture was withdrawn and preserved separately.

`VulkanRendererAPI::BeginRecording` returns a shared completion proof. Each query
rewrite replaces its previous proof. The submission owner publishes the fence
only after successful submission, and latches completion before reusing or
destroying that fence. Nonblocking reads poll it; blocking reads wait only when
the current generation has actually been submitted. A frame's final fence also
covers its ordered graphics segments and joined async-compute work.

A blocking mid-frame flush retires the old proof and gives resumed writes a new
one. A failed or discarded segment stays unreadable even if the later final
submission succeeds. Open elapsed/occlusion brackets already refuse a split.
Do not reuse the deferred-reclaim generation as a submission ticket: an aborted
swapchain acquire can advance that counter without a new submission.

The device regressions in `VulkanPassSuiteTest.cpp` establish old raw availability
while a replacement reset is still unsubmitted, then require the facade to reject
it for timestamp, elapsed, and occlusion queries. They also cover submitted
blocking reads, successful flush followed by reuse, and discarded flush followed
by successful final submission. Timestamp ordering alone cannot replace these
checks: two stale endpoints can still produce an apparently valid interval.
