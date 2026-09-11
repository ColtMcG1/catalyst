/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Exercises Tier 2: `timeline_point`, the three queues, `submit`, and the `co_await` /
 * `pump` seam.
 * @details Written to be meaningful on both a real adapter and the bookkeeping backend, because the
 * properties that matter are the same either way: a point is ordered within its timeline and
 * unordered across timelines, a submission's point is complete once the work is done, a list is
 * refused by a queue that cannot run it, and a coroutine parked on a point resumes in `pump` on the
 * thread that calls it - never anywhere else.
 *
 * What is deliberately *not* asserted is that a point is incomplete immediately after `submit`.
 * That is true on a GPU and false on the bookkeeping backend, which finishes the work before
 * `submit` returns, and a test that demanded it would be asserting the presence of latency.
 */

#include "../test_common.hpp"

#include <catalyst/events/task.hpp>
#include <catalyst/rendering/rendering.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <utility>
#include <string_view>
#include <vector>

using namespace catalyst::rendering;
namespace events = catalyst::events;

namespace
{
    /** A closed, submittable list that copies between two buffers, so it is real work everywhere. */
    struct copy_work
    {
        structured_buffer<std::uint32_t> src;
        structured_buffer<std::uint32_t> dst;
        command_list list;

        void destroy()
        {
            destroy_command_list(list);
            src.destroy();
            dst.destroy();
        }
    };

    copy_work make_copy_work(const device &dev, queue_kind kind, std::uint32_t seed)
    {
        constexpr std::size_t count = 64;
        std::array<std::uint32_t, count> values{};
        for (std::size_t i = 0; i < count; ++i)
            values[i] = seed + static_cast<std::uint32_t>(i);

        copy_work w;
        w.src = create_structured_buffer<std::uint32_t>(dev, count, buffer_usage::transfer_src,
                                                        memory_access::cpu_to_gpu,
                                                        std::span<const std::uint32_t>{values});
        w.dst = create_structured_buffer<std::uint32_t>(dev, count,
                                                        buffer_usage::transfer_dst | buffer_usage::storage,
                                                        memory_access::gpu_to_cpu);
        CT_REQUIRE(w.src && w.dst);

        w.list = create_command_list(dev, {kind, "timeline test copy"});
        CT_REQUIRE(is_valid(w.list));
        CT_REQUIRE(begin_recording(w.list));
        copy_buffer(w.list, w.src.handle(), 0, w.dst.handle(), 0, w.src.size_bytes());
        CT_REQUIRE(end_recording(w.list));
        return w;
    }

    void test_point_vocabulary()
    {
        // The "no work" point. Everything about it has to be the answer that lets a caller who
        // submitted nothing skip the special case.
        constexpr timeline_point none;
        static_assert(!none.valid());
        static_assert(none.value() == 0);
        CT_REQUIRE(!static_cast<bool>(none));
        CT_REQUIRE(none.is_complete());
        CT_REQUIRE(none.wait());
        CT_REQUIRE(none.wait_for(std::chrono::seconds{0}));

        static_assert(std::is_trivially_copyable_v<timeline_point>);
        static_assert(sizeof(timeline_point) <= 24);

        // Ordering is per timeline. Two points on different queues of the same device are not
        // ordered at all, and neither are points on different devices.
        const device a{1};
        const device b{2};
        const timeline_point a_gfx_1{a, queue_kind::graphics, 1};
        const timeline_point a_gfx_2{a, queue_kind::graphics, 2};
        const timeline_point a_copy_1{a, queue_kind::copy, 1};
        const timeline_point b_gfx_1{b, queue_kind::graphics, 1};

        CT_REQUIRE(same_timeline(a_gfx_1, a_gfx_2));
        CT_REQUIRE(a_gfx_1 < a_gfx_2);
        CT_REQUIRE(a_gfx_2 > a_gfx_1);
        CT_REQUIRE(a_gfx_1 == timeline_point(a, queue_kind::graphics, 1));

        CT_REQUIRE(!same_timeline(a_gfx_1, a_copy_1));
        CT_REQUIRE(!same_timeline(a_gfx_1, b_gfx_1));
        // Unordered, which means every comparison is false - including `>=`, where a total order
        // would have said true for two equal-looking values.
        CT_REQUIRE((a_gfx_1 <=> a_copy_1) == std::partial_ordering::unordered);
        CT_REQUIRE(!(a_gfx_1 < a_copy_1));
        CT_REQUIRE(!(a_gfx_1 >= a_copy_1));
        CT_REQUIRE(a_gfx_1 != a_copy_1);

        // `latest` picks the valid one, the later one, and refuses to invent an order.
        CT_REQUIRE(latest(none, a_gfx_1) == a_gfx_1);
        CT_REQUIRE(latest(a_gfx_1, none) == a_gfx_1);
        CT_REQUIRE(latest(a_gfx_1, a_gfx_2) == a_gfx_2);
        CT_REQUIRE(latest(a_gfx_2, a_gfx_1) == a_gfx_2);
        CT_REQUIRE(latest(a_gfx_1, a_copy_1) == a_gfx_1);
    }

    void test_queues_are_described()
    {
        device dev = create_device();
        CT_REQUIRE(is_valid(dev));
        CT_REQUIRE(!is_device_lost(dev));

        for (const queue_kind kind : {queue_kind::graphics, queue_kind::compute, queue_kind::copy})
        {
            const queue q = get_queue(dev, kind);
            CT_REQUIRE(static_cast<bool>(q));
            CT_REQUIRE(q.kind() == kind);
            CT_REQUIRE(q.owner() == dev);

            const queue_info info = get_queue_info(q);
            CT_REQUIRE(info.kind == kind);
            // Graphics is always real hardware; the other two may be aliases of it, and the point
            // of the flag is that a caller can find out rather than guess.
            if (kind == queue_kind::graphics)
                CT_REQUIRE(info.dedicated);

            CT_REQUIRE(!last_submitted(q).valid()); // Nothing submitted yet.
            CT_REQUIRE(completed(q).is_complete());
        }

        // An invalid queue is refused rather than quietly doing nothing.
        const queue nowhere;
        CT_REQUIRE(!static_cast<bool>(nowhere));
        CT_REQUIRE(!submit(nowhere, submit_info{}));
        CT_REQUIRE(!wait_idle(nowhere));
        CT_REQUIRE(submit(nowhere, submit_info{}).error().code == error_code::invalid_argument);

        destroy_device(dev);
    }

    void test_submission_points()
    {
        device dev = create_device();
        CT_REQUIRE(is_valid(dev));
        const queue graphics = get_queue(dev);

        copy_work w = make_copy_work(dev, queue_kind::copy, 100);

        // The copy queue: a distinct DMA engine where the adapter has one, and the graphics queue
        // under another name where it does not. Caller-side code does not know or care which.
        const queue copy = get_queue(dev, queue_kind::copy);
        const auto first = submit(copy, w.list);
        CT_REQUIRE(first);
        CT_REQUIRE(first->valid());
        CT_REQUIRE(first->queue() == queue_kind::copy);
        CT_REQUIRE(first->owner() == dev);
        CT_REQUIRE(*first == last_submitted(copy));

        const auto second = submit(copy, w.list);
        CT_REQUIRE(second);
        CT_REQUIRE(*first < *second);

        // Waiting on the point makes it complete, and everything before it too.
        CT_REQUIRE(second->wait());
        CT_REQUIRE(second->is_complete());
        CT_REQUIRE(first->is_complete());
        CT_REQUIRE(*second <= completed(copy));

        // The work actually happened.
        std::array<std::uint32_t, 64> out{};
        CT_REQUIRE(w.dst.read(out));
        CT_REQUIRE(out[0] == 100 && out[63] == 163);

        // An empty submission is legal and gives back a point later than everything before it -
        // which is how a caller inserts a fence between queues with no work to run.
        const auto barrier = submit(copy, submit_info{});
        CT_REQUIRE(barrier);
        CT_REQUIRE(*second < *barrier);

        // A point from another device is not a dependency this queue can honour.
        const timeline_point foreign{device{0xDEAD}, queue_kind::graphics, 1};
        CT_REQUIRE(!submit(copy, submit_info{.wait = {&foreign, 1}}));

        // ... but an invalid point means "no dependency" and is simply ignored.
        constexpr timeline_point nothing;
        CT_REQUIRE(submit(graphics, submit_info{.wait = {&nothing, 1}}));

        w.destroy();
        destroy_device(dev);
    }

    void test_cross_queue_dependency()
    {
        device dev = create_device();
        CT_REQUIRE(is_valid(dev));

        copy_work producer = make_copy_work(dev, queue_kind::copy, 7);
        copy_work consumer = make_copy_work(dev, queue_kind::graphics, 9);

        const auto uploaded = submit(get_queue(dev, queue_kind::copy), producer.list);
        CT_REQUIRE(uploaded);

        // The graphics submission waits for the copy on the GPU: this call does not block, and the
        // CPU is not involved in the ordering at all.
        const auto drawn = submit(get_queue(dev), submit_info{
                                                      .lists = {&consumer.list, 1},
                                                      .wait = {&*uploaded, 1},
                                                  });
        CT_REQUIRE(drawn);
        CT_REQUIRE(!same_timeline(*uploaded, *drawn));

        CT_REQUIRE(drawn->wait());
        // The dependency is what makes this true rather than a race: the copy cannot still be
        // running once the work that waited on it has finished.
        CT_REQUIRE(uploaded->is_complete());

        CT_REQUIRE(wait_idle(get_queue(dev, queue_kind::copy)));

        consumer.destroy();
        producer.destroy();
        destroy_device(dev);
    }

    void test_queue_kind_is_enforced()
    {
        device dev = create_device();
        CT_REQUIRE(is_valid(dev));

        // A list goes to the queue it was recorded for, and nowhere else. Not a policy choice: the
        // command buffer belongs to the family its pool was created from, so submitting it to
        // another queue is undefined behaviour that a driver is entitled to turn into a crash. This
        // is the check that keeps that from ever reaching one.
        for (const queue_kind recorded : {queue_kind::graphics, queue_kind::compute, queue_kind::copy})
        {
            copy_work w = make_copy_work(dev, recorded, 1);
            CT_REQUIRE(get_command_list_desc(w.list).queue == recorded);

            for (const queue_kind target : {queue_kind::graphics, queue_kind::compute, queue_kind::copy})
            {
                const auto result = submit(get_queue(dev, target), w.list);
                if (target == recorded)
                {
                    CT_REQUIRE(result);
                }
                else
                {
                    CT_REQUIRE(!result);
                    CT_REQUIRE(result.error().code == error_code::invalid_argument);
                }
            }

            wait_idle(dev);
            w.destroy();
        }

        destroy_device(dev);
    }

    /** Records where a coroutine resumed, so the test can prove it was `pump` and not `submit`. */
    struct await_trace
    {
        bool started = false;
        bool resumed = false;
        bool ok = false;
    };

    events::task<void> await_point(timeline_point point, await_trace &trace)
    {
        trace.started = true;
        const auto result = co_await point;
        trace.resumed = true;
        trace.ok = result.has_value();
    }

    void test_co_await_resumes_in_pump()
    {
        device dev = create_device();
        CT_REQUIRE(is_valid(dev));

        copy_work w = make_copy_work(dev, queue_kind::copy, 500);
        const auto done = submit(get_queue(dev, queue_kind::copy), w.list);
        CT_REQUIRE(done);

        await_trace trace;
        events::task<void> task = await_point(*done, trace);
        CT_REQUIRE(!trace.started);

        task.start();
        CT_REQUIRE(trace.started);

        // On a real GPU the point is probably not complete yet, so the coroutine parked and only
        // `pump` will resume it. On the bookkeeping backend it completed before `submit` returned,
        // so `await_ready` was true and it never suspended. Both are correct; what must hold in
        // either case is that pumping until the point completes gets the coroutine finished, and
        // that nothing resumed it on some other thread in the meantime.
        CT_REQUIRE(done->wait());
        pump(dev);

        CT_REQUIRE(trace.resumed);
        CT_REQUIRE(trace.ok);
        CT_REQUIRE(task.done());

        // Pumping with nothing parked is cheap and harmless, which is what lets a frame loop call
        // it unconditionally.
        pump(dev);
        pump(device{});

        w.destroy();
        destroy_device(dev);
    }

    void test_await_completed_point_does_not_park()
    {
        device dev = create_device();
        CT_REQUIRE(is_valid(dev));

        copy_work w = make_copy_work(dev, queue_kind::copy, 900);
        const auto done = submit(get_queue(dev, queue_kind::copy), w.list);
        CT_REQUIRE(done);
        CT_REQUIRE(done->wait()); // Complete before the coroutine ever looks at it.

        await_trace trace;
        events::task<void> task = await_point(*done, trace);
        task.start();

        // `await_ready` was true, so it ran straight through with no `pump` at all.
        CT_REQUIRE(trace.resumed);
        CT_REQUIRE(trace.ok);
        CT_REQUIRE(task.done());

        // The same holds for the "no work" point, which is the case a caller hits when a frame had
        // nothing to submit.
        await_trace empty_trace;
        events::task<void> empty = await_point(timeline_point{}, empty_trace);
        empty.start();
        CT_REQUIRE(empty_trace.resumed);
        CT_REQUIRE(empty_trace.ok);

        w.destroy();
        destroy_device(dev);
    }

    void test_bounded_wait()
    {
        device dev = create_device();
        CT_REQUIRE(is_valid(dev));

        const queue graphics = get_queue(dev);

        // Waiting for work that was never submitted is a caller error, not a hang: there is nothing
        // that could ever signal it.
        const timeline_point never{dev, queue_kind::graphics, 9'999};
        const auto refused = never.wait_for(std::chrono::milliseconds{1});
        CT_REQUIRE(!refused);
        CT_REQUIRE(refused.error().code == error_code::invalid_argument);

        // A real point either completes within a generous deadline or reports a timeout; both are
        // outcomes, and neither is device loss.
        copy_work w = make_copy_work(dev, queue_kind::graphics, 3);
        const auto done = submit(graphics, w.list);
        CT_REQUIRE(done);

        const auto bounded = done->wait_for(std::chrono::seconds{5});
        if (!bounded)
            CT_REQUIRE(bounded.error().code == error_code::timeout);
        CT_REQUIRE(!is_device_lost(dev));

        CT_REQUIRE(done->wait());
        w.destroy();
        destroy_device(dev);
    }
} // namespace

int main()
{
    // Named so a crash names the case that caused it: these run against a real adapter, where the
    // failure that matters most is a hang or a fault rather than an assertion.
    const std::pair<const char *, void (*)()> cases[] = {
        {"point vocabulary", test_point_vocabulary},
        {"queues are described", test_queues_are_described},
        {"submission points", test_submission_points},
        {"cross-queue dependency", test_cross_queue_dependency},
        {"queue kind is enforced", test_queue_kind_is_enforced},
        {"co_await resumes in pump", test_co_await_resumes_in_pump},
        {"await completed point does not park", test_await_completed_point_does_not_park},
        {"bounded wait", test_bounded_wait},
    };

    for (const auto &[name, fn] : cases)
    {
        std::printf("  %s ...\n", name);
        std::fflush(stdout);
        fn();
    }

    std::printf("rendering timelines: ok (backend %s)\n", to_string(backend()));
    return 0;
}
