/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Exercises Tier 3: command pools, the recycling rules that replaced the hidden wait in
 * `begin_recording`, `frame_ring`, and recording from several threads at once.
 * @details Meaningful on a real adapter and on the bookkeeping backend both, because the properties
 * being asserted are structural rather than timing-dependent: a pool hands out lists of its own
 * kind, a list may be recorded once per reset cycle, a ring hands out `frames_in_flight` distinct
 * slots and comes back round to the first, and N threads recording into N pools produce N
 * submittable lists without corrupting each other.
 *
 * What is deliberately *not* asserted is that `begin_recording` fails on a list still in flight.
 * That is true on a GPU and false on the bookkeeping backend, which finishes the work before
 * `submit` returns - a test demanding it would be asserting the presence of latency. What *is*
 * asserted is the rule that holds everywhere: without a `reset_command_pool`, a second recording is
 * refused.
 */

#include "../test_common.hpp"

#include "validation_option.hpp"

#include <catalyst/rendering/rendering.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using namespace catalyst::rendering;

namespace
{
    device make_device()
    {
        device dev = create_device(
            catalyst::tests::with_validation({.application_name = "catalyst frame tests"}));
        CT_REQUIRE(is_valid(dev));
        return dev;
    }

    /** Records a trivially valid list: a buffer-to-buffer copy, which every queue kind accepts. */
    struct copy_work
    {
        structured_buffer<std::uint32_t> src;
        structured_buffer<std::uint32_t> dst;

        void record(const command_list &cl) const
        {
            copy_buffer(cl, src.handle(), 0, dst.handle(), 0, src.size_bytes());
        }

        void destroy()
        {
            src.destroy();
            dst.destroy();
        }
    };

    copy_work make_copy_work(const device &dev)
    {
        constexpr std::size_t count = 16;
        std::array<std::uint32_t, count> values{};
        for (std::size_t i = 0; i < count; ++i)
            values[i] = static_cast<std::uint32_t>(i);

        copy_work w;
        w.src = create_structured_buffer<std::uint32_t>(dev, count, buffer_usage::transfer_src,
                                                        memory_access::cpu_to_gpu,
                                                        std::span<const std::uint32_t>{values});
        w.dst = create_structured_buffer<std::uint32_t>(dev, count, buffer_usage::transfer_dst,
                                                        memory_access::gpu_only);
        CT_REQUIRE(w.src && w.dst);
        return w;
    }

    // -------------------------------------------------------------------------

    void test_pool_lifetime()
    {
        device dev = make_device();

        command_pool pool = create_command_pool(dev, {.queue = queue_kind::graphics, .debug_name = "pool"});
        CT_REQUIRE(is_valid(pool));
        CT_REQUIRE(get_command_pool_desc(pool).queue == queue_kind::graphics);
        CT_REQUIRE(get_device(pool) == dev);

        // A list from a pool takes the pool's kind, whatever the caller might have wanted. That is
        // not a policy choice: the command buffer belongs to the family the pool came from.
        command_list cl = create_command_list(pool, "from pool");
        CT_REQUIRE(is_valid(cl));
        CT_REQUIRE(get_command_list_desc(cl).queue == queue_kind::graphics);

        command_pool copy_pool = create_command_pool(dev, {.queue = queue_kind::copy});
        CT_REQUIRE(is_valid(copy_pool));
        command_list copy_list = create_command_list(copy_pool, "copy list");
        CT_REQUIRE(get_command_list_desc(copy_list).queue == queue_kind::copy);

        // Destroying the pool takes its lists with it: they are allocations from it, and freeing
        // the pool frees them whether the caller asked or not, so the handles have to go stale.
        destroy_command_pool(pool);
        CT_REQUIRE(!pool);
        CT_REQUIRE(!is_valid(cl));

        destroy_command_pool(copy_pool);
        CT_REQUIRE(!is_valid(copy_list));

        // Invalid handles fail rather than doing nothing quietly.
        CT_REQUIRE(!reset_command_pool(command_pool{}));
        CT_REQUIRE(reset_command_pool(command_pool{}).error().code == error_code::invalid_argument);
        CT_REQUIRE(!is_valid(create_command_list(command_pool{}, "nowhere")));

        destroy_device(dev);
    }

    void test_recording_needs_a_reset()
    {
        device dev = make_device();
        copy_work w = make_copy_work(dev);

        command_pool pool = create_command_pool(dev, {.queue = queue_kind::graphics});
        command_list cl = create_command_list(pool, "recycled");
        CT_REQUIRE(is_valid(cl));

        CT_REQUIRE(begin_recording(cl));
        w.record(cl);
        CT_REQUIRE(end_recording(cl));

        // The rule the pool model imposes, and the one that replaced the hidden wait: a list is
        // recorded once per reset cycle. Asking twice is refused rather than silently producing a
        // command buffer the driver has already been handed.
        CT_REQUIRE(!begin_recording(cl));

        const auto done = submit(get_queue(dev), cl);
        CT_REQUIRE(done);
        CT_REQUIRE(last_submission(cl) == *done);
        CT_REQUIRE(done->wait());

        // After the wait the pool may be recycled, and the list is recordable again - with its
        // submission point cleared, because the list no longer holds anything the GPU is reading.
        CT_REQUIRE(reset_command_pool(pool));
        CT_REQUIRE(!last_submission(cl).valid());
        CT_REQUIRE(begin_recording(cl));
        CT_REQUIRE(end_recording(cl));

        destroy_command_pool(pool);
        w.destroy();
        destroy_device(dev);
    }

    void test_implicit_pool_recycles_itself()
    {
        device dev = make_device();
        copy_work w = make_copy_work(dev);

        // The convenience form owns its pool, so it may recycle it in `begin_recording` - there is
        // nothing else in the pool for that to disturb. This is what keeps every caller written
        // before Tier 3 working, minus the wait.
        command_list cl = create_command_list(dev, {.queue = queue_kind::graphics, .debug_name = "standalone"});
        CT_REQUIRE(is_valid(cl));

        for (int i = 0; i < 3; ++i)
        {
            CT_REQUIRE(begin_recording(cl));
            w.record(cl);
            CT_REQUIRE(end_recording(cl));

            const auto done = submit(get_queue(dev), cl);
            CT_REQUIRE(done);
            CT_REQUIRE(done->wait()); // What `begin_recording` used to do invisibly, said out loud.
        }

        destroy_command_list(cl);
        CT_REQUIRE(!cl);
        w.destroy();
        destroy_device(dev);
    }

    void test_ring_cycles_slots()
    {
        device dev = make_device();

        auto ring = frame_ring::create(dev, {.frames_in_flight = 3, .workers = 2, .debug_name = "test"});
        CT_REQUIRE(ring);
        CT_REQUIRE(ring->frames_in_flight() == 3);
        CT_REQUIRE(ring->workers() == 2);
        CT_REQUIRE(ring->owner() == dev);

        // Slots come round in order and repeat with the ring's period; the index keeps counting.
        for (std::uint64_t i = 0; i < 7; ++i)
        {
            auto f = ring->begin();
            CT_REQUIRE(f);
            CT_REQUIRE(f->index() == i);
            CT_REQUIRE(f->slot() == static_cast<std::uint32_t>(i % 3));

            // A pool per worker per kind, all distinct, all belonging to this device.
            CT_REQUIRE(is_valid(f->pool(0)));
            CT_REQUIRE(is_valid(f->pool(1)));
            CT_REQUIRE(f->pool(0) != f->pool(1));
            CT_REQUIRE(f->pool(0, queue_kind::copy) != f->pool(0, queue_kind::graphics));
            CT_REQUIRE(get_command_pool_desc(f->pool(0, queue_kind::copy)).queue == queue_kind::copy);

            // Out of range is an invalid handle, not a wrapped-around one: a worker index the ring
            // was not sized for is a bug, and quietly aliasing another worker's pool would hide it.
            CT_REQUIRE(!is_valid(f->pool(2)));

            f->end();
        }
        CT_REQUIRE(ring->frame_index() == 7);

        // Clamping, so a caller cannot ask for a ring that cannot exist.
        auto clamped = frame_ring::create(dev, {.frames_in_flight = 0, .workers = 0});
        CT_REQUIRE(clamped);
        CT_REQUIRE(clamped->frames_in_flight() == 1);
        CT_REQUIRE(clamped->workers() == 1);

        CT_REQUIRE(!frame_ring::create(device{}));

        *clamped = frame_ring{};
        *ring = frame_ring{};
        destroy_device(dev);
    }

    void test_ring_recycles_pools()
    {
        device dev = make_device();
        copy_work w = make_copy_work(dev);

        // One frame in flight is the worst case for the ring - every `begin` waits for the frame
        // before it - and the case that proves the recycling actually happens, because the same
        // pool is handed out every time.
        auto ring = frame_ring::create(dev, {.frames_in_flight = 1});
        CT_REQUIRE(ring);

        command_list cl{};
        for (int i = 0; i < 4; ++i)
        {
            auto f = ring->begin();
            CT_REQUIRE(f);
            CT_REQUIRE(f->slot() == 0);

            if (!cl)
                cl = create_command_list(f->pool(), "ring frame");
            CT_REQUIRE(is_valid(cl));

            // Recordable every time round only because `begin` reset the pool first.
            CT_REQUIRE(begin_recording(cl));
            w.record(cl);
            CT_REQUIRE(end_recording(cl));

            const auto done = submit(get_queue(dev), cl);
            CT_REQUIRE(done);
            f->end(*done);
        }

        CT_REQUIRE(ring->wait_all());
        destroy_command_list(cl);
        *ring = frame_ring{};
        w.destroy();
        destroy_device(dev);
    }

    void test_parallel_recording()
    {
        constexpr std::uint32_t workers = 4;

        device dev = make_device();
        copy_work w = make_copy_work(dev);

        auto ring = frame_ring::create(dev, {.frames_in_flight = 2, .workers = workers});
        CT_REQUIRE(ring);

        for (int pass = 0; pass < 3; ++pass)
        {
            auto f = ring->begin();
            CT_REQUIRE(f);

            std::vector<command_list> lists(workers);
            {
                // The shape Tier 3 exists for: worker i touches pool i and nothing else, so there
                // is no shared mutable state on the recording path to lock. Everything below runs
                // concurrently - including `create_command_list`, which does take the module lock,
                // so this covers the create-while-recording race too.
                std::vector<std::thread> threads;
                threads.reserve(workers);
                for (std::uint32_t i = 0; i < workers; ++i)
                {
                    threads.emplace_back([&, i] {
                        command_list cl = create_command_list(f->pool(i), "worker");
                        if (!is_valid(cl) || !begin_recording(cl))
                            return;
                        w.record(cl);
                        if (!end_recording(cl))
                            return;
                        lists[i] = cl;
                    });
                }
                for (std::thread &t : threads)
                    t.join();
            }

            for (const command_list &cl : lists)
                CT_REQUIRE(is_valid(cl));

            // One thread submits what N produced, which is the division of labour the header
            // describes.
            const auto done = submit(get_queue(dev), submit_info{.lists = lists});
            CT_REQUIRE(done);
            f->end(*done);

            CT_REQUIRE(done->wait());
            for (command_list &cl : lists)
                destroy_command_list(cl);
        }

        *ring = frame_ring{};
        w.destroy();
        destroy_device(dev);
    }

    void test_concurrent_creation()
    {
        // Creation and destruction from several threads at once is defined from Tier 3 on. There is
        // nothing to assert beyond "it finished and every handle is real": what this case is really
        // for is the sanitiser and the crash that a racing `unordered_map` rehash used to produce.
        device dev = make_device();

        constexpr std::uint32_t threads_count = 4;
        constexpr std::size_t per_thread = 32;

        std::vector<std::vector<structured_buffer<std::uint32_t>>> made(threads_count);
        {
            std::vector<std::thread> threads;
            threads.reserve(threads_count);
            for (std::uint32_t t = 0; t < threads_count; ++t)
            {
                threads.emplace_back([&, t] {
                    for (std::size_t i = 0; i < per_thread; ++i)
                    {
                        made[t].push_back(create_structured_buffer<std::uint32_t>(
                            dev, 64, buffer_usage::storage, memory_access::cpu_to_gpu));
                    }
                });
            }
            for (std::thread &t : threads)
                t.join();
        }

        for (auto &group : made)
        {
            CT_REQUIRE(group.size() == per_thread);
            for (auto &b : group)
                CT_REQUIRE(b);
        }

        {
            std::vector<std::thread> threads;
            threads.reserve(threads_count);
            for (std::uint32_t t = 0; t < threads_count; ++t)
                threads.emplace_back([&, t] {
                    for (auto &b : made[t])
                        b.destroy();
                });
            for (std::thread &t : threads)
                t.join();
        }

        destroy_device(dev);
    }
} // namespace

int main()
{
    const std::pair<const char *, void (*)()> cases[] = {
        {"pool lifetime", test_pool_lifetime},
        {"recording needs a reset", test_recording_needs_a_reset},
        {"implicit pool recycles itself", test_implicit_pool_recycles_itself},
        {"ring cycles slots", test_ring_cycles_slots},
        {"ring recycles pools", test_ring_recycles_pools},
        {"parallel recording", test_parallel_recording},
        {"concurrent creation", test_concurrent_creation},
    };

    for (const auto &[name, fn] : cases)
    {
        std::printf("  %s ...\n", name);
        std::fflush(stdout);
        fn();
    }

    std::printf("rendering frames: ok (backend %s)\n", to_string(backend()));
    return 0;
}
