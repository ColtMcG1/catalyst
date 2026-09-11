/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Exercises Tier 4: `transfer_batch`, the staging ring and its back-pressure, `download` /
 * `readback`, and the ordering promise that lets an asynchronous `write_buffer` be correct without
 * the caller doing anything about it.
 * @details The bytes are the assertion. Every case here uploads a known pattern and reads it back,
 * which is the one check that is equally meaningful on a real adapter (where the copy happened on
 * a separate engine and had to be ordered) and on the bookkeeping backend (where it was a memcpy).
 *
 * The one property that cannot be asserted portably is that an upload is *not yet* visible when
 * `submit` returns: that is true on a GPU and false without one, and demanding it would be
 * asserting the presence of latency.
 */

#include "../test_common.hpp"

#include "validation_option.hpp"

#include <catalyst/events/task.hpp>
#include <catalyst/rendering/rendering.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

using namespace catalyst::rendering;
namespace events = catalyst::events;

namespace
{
    device make_device(std::uint64_t ring_bytes = 0)
    {
        device dev = create_device(
            catalyst::tests::with_validation({.application_name = "catalyst transfer tests",
                                              .staging_ring_bytes = ring_bytes}));
        CT_REQUIRE(is_valid(dev));
        return dev;
    }

    std::vector<std::uint32_t> pattern(std::size_t count, std::uint32_t seed)
    {
        std::vector<std::uint32_t> values(count);
        for (std::size_t i = 0; i < count; ++i)
            values[i] = seed + static_cast<std::uint32_t>(i) * 2654435761u;
        return values;
    }

    std::span<const std::byte> as_bytes(const std::vector<std::uint32_t> &values)
    {
        return std::as_bytes(std::span{values});
    }

    /** A GPU-only buffer that can be uploaded into and read back out of. */
    buffer make_target(const device &dev, std::size_t bytes)
    {
        buffer b = create_buffer(dev, {.size_bytes = bytes,
                                       .usage = buffer_usage::transfer_dst | buffer_usage::transfer_src |
                                                buffer_usage::storage,
                                       .access = memory_access::gpu_only,
                                       .debug_name = "transfer target"});
        CT_REQUIRE(is_valid(b));
        return b;
    }

    /** Downloads `count` uint32s from `src` and blocks for them. */
    std::vector<std::uint32_t> read_back(const device &dev, const buffer &src, std::size_t count)
    {
        auto rb = download(dev, src, 0, count * sizeof(std::uint32_t));
        CT_REQUIRE(rb);
        CT_REQUIRE(rb->wait());

        const auto bytes = rb->bytes();
        CT_REQUIRE(bytes);
        CT_REQUIRE(bytes->size() == count * sizeof(std::uint32_t));

        std::vector<std::uint32_t> out(count);
        std::memcpy(out.data(), bytes->data(), bytes->size());
        return out;
    }

    // -------------------------------------------------------------------------

    void test_staging_info()
    {
        // The ring is sized where the caller can see it, and reports what it can never carry.
        device dev = make_device(1u << 20);
        const staging_info info = get_staging_info(dev);
        CT_REQUIRE(info.capacity_bytes >= 1u << 20);
        CT_REQUIRE(info.largest_transfer_bytes == info.capacity_bytes);
        CT_REQUIRE(info.in_use_bytes <= info.capacity_bytes);
        destroy_device(dev);

        CT_REQUIRE(get_staging_info(device{}).capacity_bytes == 0);
    }

    void test_batch_uploads_and_reads_back()
    {
        constexpr std::size_t count = 512;
        device dev = make_device();

        const std::vector<std::uint32_t> values = pattern(count, 11);
        buffer target = make_target(dev, count * sizeof(std::uint32_t));

        auto batch = transfer_batch::begin(dev);
        CT_REQUIRE(batch);
        CT_REQUIRE(batch->owner() == dev);
        CT_REQUIRE(batch->size() == 0);

        CT_REQUIRE(batch->upload(target, 0, as_bytes(values)));
        CT_REQUIRE(batch->size() == 1);

        const auto resident = batch->submit();
        CT_REQUIRE(resident);
        // Transfers run on the copy queue, whatever the adapter chose to make that.
        CT_REQUIRE(!resident->valid() || resident->queue() == queue_kind::copy);
        CT_REQUIRE(batch->size() == 0); // Submitting empties it; it may be filled again.

        CT_REQUIRE(read_back(dev, target, count) == values);

        destroy_buffer(target);
        destroy_device(dev);
    }

    void test_many_uploads_in_one_submission()
    {
        constexpr std::size_t chunks = 32;
        constexpr std::size_t per_chunk = 64;
        device dev = make_device();

        const std::vector<std::uint32_t> values = pattern(chunks * per_chunk, 77);
        buffer target = make_target(dev, values.size() * sizeof(std::uint32_t));

        auto batch = transfer_batch::begin(dev);
        CT_REQUIRE(batch);

        // The point of a batch: N copies, one submission. Each chunk is staged as it is recorded,
        // so the caller's vector need not outlive the call.
        for (std::size_t i = 0; i < chunks; ++i)
        {
            const std::span<const std::uint32_t> chunk{values.data() + i * per_chunk, per_chunk};
            CT_REQUIRE(batch->upload(target, i * per_chunk * sizeof(std::uint32_t), std::as_bytes(chunk)));
        }
        CT_REQUIRE(batch->size() == chunks);

        const auto resident = batch->submit();
        CT_REQUIRE(resident);
        CT_REQUIRE(read_back(dev, target, values.size()) == values);

        destroy_buffer(target);
        destroy_device(dev);
    }

    void test_write_buffer_is_ordered_without_help()
    {
        // The promise that keeps every caller written before Tier 4 correct: `write_buffer` no
        // longer waits for the GPU, and the read that follows still sees the bytes, because later
        // submissions wait on the transfer themselves.
        constexpr std::size_t count = 256;
        device dev = make_device();

        buffer target = make_target(dev, count * sizeof(std::uint32_t));
        const std::vector<std::uint32_t> values = pattern(count, 5);

        CT_REQUIRE(write_buffer(target, 0, as_bytes(values)));
        CT_REQUIRE(read_back(dev, target, count) == values);

        // And again, over a buffer that queues have already seen - the write-after-read direction,
        // which needs the transfer ordered *after* the work that might still be reading it.
        const std::vector<std::uint32_t> replacement = pattern(count, 999);
        CT_REQUIRE(write_buffer(target, 0, as_bytes(replacement)));
        CT_REQUIRE(read_back(dev, target, count) == replacement);

        destroy_buffer(target);
        destroy_device(dev);
    }

    void test_initial_data_is_asynchronous()
    {
        // `create_buffer` with initial data used to be a full CPU/GPU round trip each. It is now a
        // staged copy on the copy queue, and the data is still there when anyone looks.
        constexpr std::size_t count = 128;
        device dev = make_device();

        std::vector<buffer> buffers;
        std::vector<std::vector<std::uint32_t>> expected;
        for (std::uint32_t i = 0; i < 8; ++i)
        {
            expected.push_back(pattern(count, i * 31u + 1u));
            buffer b = create_buffer(dev, {.size_bytes = count * sizeof(std::uint32_t),
                                           .usage = buffer_usage::transfer_dst | buffer_usage::transfer_src,
                                           .access = memory_access::gpu_only},
                                     as_bytes(expected.back()));
            CT_REQUIRE(is_valid(b));
            buffers.push_back(b);
        }

        for (std::size_t i = 0; i < buffers.size(); ++i)
            CT_REQUIRE(read_back(dev, buffers[i], count) == expected[i]);

        for (buffer &b : buffers)
            destroy_buffer(b);
        destroy_device(dev);
    }

    void test_one_shot_upload()
    {
        constexpr std::size_t count = 64;
        device dev = make_device();

        buffer target = make_target(dev, count * sizeof(std::uint32_t));
        const std::vector<std::uint32_t> values = pattern(count, 42);

        const auto resident = upload(dev, target, 0, as_bytes(values));
        CT_REQUIRE(resident);
        CT_REQUIRE(resident->wait());
        CT_REQUIRE(read_back(dev, target, count) == values);

        destroy_buffer(target);
        destroy_device(dev);
    }

    /** A texture whose top mip is `bytes` of tightly packed RGBA8. */
    texture make_texture(const device &dev, std::uint32_t width, std::uint32_t height)
    {
        texture t = create_texture(dev, {.dimension = texture_dimension::texture_2d,
                                         .extent = {width, height, 1},
                                         .pixel_format = format::rgba8_unorm,
                                         .usage = texture_usage::sampled | texture_usage::transfer_dst,
                                         .debug_name = "transfer target"});
        CT_REQUIRE(is_valid(t));
        return t;
    }

    void test_staging_exhaustion()
    {
        // Textures rather than buffers, deliberately. A GPU-only *buffer* on a unified-memory
        // adapter is host-visible, so an upload into it is a memcpy that never touches the ring -
        // correct, and useless for testing the ring. An image is never host-visible, so this path
        // is the same everywhere.
        device dev = make_device(64 * 1024);
        const staging_info info = get_staging_info(dev);
        CT_REQUIRE(info.largest_transfer_bytes != 0);

        // Twice the ring, so no amount of waiting could ever make room.
        const std::uint32_t side = 256; // 256 * 256 * 4 == 256 KiB.
        texture big = make_texture(dev, side, side);
        const std::vector<std::byte> oversized(static_cast<std::size_t>(side) * side * 4, std::byte{0x7f});
        CT_REQUIRE(oversized.size() > info.largest_transfer_bytes);

        auto batch = transfer_batch::begin(dev);
        CT_REQUIRE(batch);

        const auto refused = batch->upload(big, std::span<const std::byte>{oversized});
        CT_REQUIRE(!refused);
        CT_REQUIRE(refused.error().code == error_code::staging_exhausted);
        // Transient, because most of the time it clears - just not this time.
        CT_REQUIRE(is_transient(refused.error().code));

        // The batch survives a refusal; something that fits still works.
        texture small = make_texture(dev, 32, 32);
        const std::vector<std::byte> fits(32 * 32 * 4, std::byte{0x11});
        CT_REQUIRE(batch->upload(small, std::span<const std::byte>{fits}));
        CT_REQUIRE(batch->submit());
        CT_REQUIRE(wait_idle(get_queue(dev, queue_kind::copy)));

        destroy_texture(small);
        destroy_texture(big);
        destroy_device(dev);
    }

    void test_ring_wraps()
    {
        // Many uploads through a small ring. If the wrap arithmetic were wrong this either
        // corrupts a payload or wedges on `staging_exhausted` and never recovers.
        device dev = make_device(64 * 1024);

        // 16 KiB an upload, so the 64 KiB ring turns over every four passes.
        texture t = make_texture(dev, 64, 64);
        const std::vector<std::byte> pixels(64 * 64 * 4, std::byte{0x5a});

        for (std::uint32_t pass = 0; pass < 64; ++pass)
        {
            auto resident = upload(dev, t, std::span<const std::byte>{pixels});
            if (!resident && resident.error().code == error_code::staging_exhausted)
            {
                // Legitimate: the copy queue is behind. Let it catch up and try once more, which
                // is the response the header recommends and has to actually work.
                CT_REQUIRE(wait_idle(get_queue(dev, queue_kind::copy)));
                resident = upload(dev, t, std::span<const std::byte>{pixels});
            }
            CT_REQUIRE(resident);
        }
        CT_REQUIRE(wait_idle(get_queue(dev, queue_kind::copy)));
        destroy_texture(t);

        // The same wrap pressure on the buffer path, with the bytes checked. On an adapter where
        // GPU-only memory is host-visible this is a memcpy rather than a staged copy - which is
        // exactly what it should be, and the bytes still have to be right.
        constexpr std::size_t count = 1024;
        buffer target = make_target(dev, count * sizeof(std::uint32_t));
        for (std::uint32_t pass = 0; pass < 24; ++pass)
        {
            const std::vector<std::uint32_t> values = pattern(count, pass * 7919u + 1u);
            auto resident = upload(dev, target, 0, as_bytes(values));
            if (!resident && resident.error().code == error_code::staging_exhausted)
            {
                CT_REQUIRE(wait_idle(get_queue(dev, queue_kind::copy)));
                resident = upload(dev, target, 0, as_bytes(values));
            }
            CT_REQUIRE(resident);
            CT_REQUIRE(read_back(dev, target, count) == values);
        }

        destroy_buffer(target);
        destroy_device(dev);
    }

    void test_readback_states()
    {
        constexpr std::size_t count = 32;
        device dev = make_device();

        buffer target = make_target(dev, count * sizeof(std::uint32_t));
        const std::vector<std::uint32_t> values = pattern(count, 13);
        CT_REQUIRE(write_buffer(target, 0, as_bytes(values)));

        auto rb = download(dev, target, 0, count * sizeof(std::uint32_t));
        CT_REQUIRE(rb);
        CT_REQUIRE(rb->valid());
        CT_REQUIRE(rb->size_bytes() == count * sizeof(std::uint32_t));
        CT_REQUIRE(rb->point().queue() == queue_kind::copy);

        // Before it has arrived, `bytes` is "not ready" rather than a lie or a hidden wait.
        if (!rb->is_complete())
            CT_REQUIRE(rb->bytes().error().code == error_code::not_ready);

        CT_REQUIRE(rb->wait());
        CT_REQUIRE(rb->is_complete());
        CT_REQUIRE(rb->bytes());

        // Moving one moves the storage the span points into with it.
        readback moved = std::move(*rb);
        CT_REQUIRE(moved.valid());
        CT_REQUIRE(!rb->valid());
        const auto bytes = moved.bytes();
        CT_REQUIRE(bytes);
        CT_REQUIRE(std::memcmp(bytes->data(), values.data(), bytes->size()) == 0);

        // A buffer without transfer_src cannot be the source of a copy, and the request is refused
        // rather than producing an empty readback.
        buffer opaque = create_buffer(dev, {.size_bytes = 64, .usage = buffer_usage::storage,
                                            .access = memory_access::gpu_only});
        CT_REQUIRE(is_valid(opaque));
        const auto refused = download(dev, opaque, 0, 64);
        CT_REQUIRE(!refused);
        CT_REQUIRE(refused.error().code == error_code::invalid_argument);

        destroy_buffer(opaque);
        destroy_buffer(target);
        destroy_device(dev);
    }

    void test_co_await_readback()
    {
        constexpr std::size_t count = 64;
        device dev = make_device();

        buffer target = make_target(dev, count * sizeof(std::uint32_t));
        const std::vector<std::uint32_t> values = pattern(count, 21);
        CT_REQUIRE(write_buffer(target, 0, as_bytes(values)));

        bool resumed = false;
        std::vector<std::uint32_t> seen;

        auto task = [&]() -> events::task<void> {
            auto rb = download(dev, target, 0, count * sizeof(std::uint32_t));
            CT_REQUIRE(rb);

            const auto bytes = co_await *rb;
            CT_REQUIRE(bytes);

            seen.resize(count);
            std::memcpy(seen.data(), bytes->data(), bytes->size());
            resumed = true;
            co_return;
        }();

        // `events::task` is lazy, so nothing has happened yet - the download has not even been
        // issued.
        CT_REQUIRE(!resumed);
        task.start();

        // The seam: a parked continuation resumes inside `pump`, on the thread that calls it, and
        // nowhere else. Bounded so a failure is a test failure rather than a hang.
        for (int i = 0; i < 1000 && !resumed; ++i)
            pump(dev);

        CT_REQUIRE(resumed);
        CT_REQUIRE(task.done());
        CT_REQUIRE(seen == values);

        destroy_buffer(target);
        destroy_device(dev);
    }

    /**
     * A task that is dropped while its `co_await` is parked must take its continuation out of the
     * park list on the way down. Without that, `pump` still holds the handle of a coroutine frame
     * that `~task` has freed, and resumes it - a use-after-free that ASan catches and a release
     * build turns into a crash somewhere else entirely.
     */
    void test_dropping_a_parked_download_does_not_strand_it()
    {
        constexpr std::size_t count = 32;

        device dev = make_device();
        buffer target = make_target(dev, count * sizeof(std::uint32_t));
        const std::vector<std::uint32_t> values = pattern(count, 11);
        CT_REQUIRE(write_buffer(target, 0, as_bytes(values)));

        const std::size_t parked_before = detail::parked_count();

        bool resumed = false;
        {
            auto task = [&]() -> events::task<void> {
                auto rb = download(dev, target, 0, count * sizeof(std::uint32_t));
                CT_REQUIRE(rb);
                (void)co_await *rb;
                resumed = true;
            }();

            task.start();

            // On a real adapter the copy is still in flight and the coroutine is parked; on the
            // bookkeeping backend it completed at submit and ran straight through. Only the first
            // case is the one under test.
            const bool parked = !task.done();
            CT_REQUIRE(parked == (detail::parked_count() == parked_before + 1));

            // Dropped here, still parked.
        }

        // The assertion that does not depend on a sanitiser: the entry is gone. Without the
        // awaiter's destructor it would still be in the list, holding a handle to the frame
        // `~task` just freed, and the next `pump` would resume it.
        CT_REQUIRE(detail::parked_count() == parked_before);

        // Only a resumption *after* the drop is interesting, so forget whatever happened above.
        resumed = false;

        pump(dev);
        pump(dev);

        CT_REQUIRE(!resumed);

        destroy_buffer(target);
        destroy_device(dev);
    }

    void test_invalid_arguments()
    {
        device dev = make_device();
        buffer target = make_target(dev, 256);

        CT_REQUIRE(!transfer_batch::begin(device{}));

        // A default-constructed batch is not a usable one, and says so rather than doing nothing.
        transfer_batch empty;
        CT_REQUIRE(!empty.valid());
        CT_REQUIRE(empty.staged_bytes() == 0);
        const std::byte byte{};
        CT_REQUIRE(!empty.upload(target, 0, std::span{&byte, 1}));
        CT_REQUIRE(!empty.submit());

        auto batch = transfer_batch::begin(dev);
        CT_REQUIRE(batch);

        // Out of bounds is refused before anything is staged.
        const std::vector<std::uint32_t> values = pattern(256, 1);
        CT_REQUIRE(batch->upload(target, 0, as_bytes(values)).error().code == error_code::invalid_argument);
        CT_REQUIRE(batch->upload(target, 4096, std::span{&byte, 1}).error().code == error_code::invalid_argument);
        CT_REQUIRE(!batch->upload(buffer{}, 0, std::span{&byte, 1}));

        // An empty upload is a no-op that succeeds: a caller looping over spans should not have to
        // special-case the empty one.
        CT_REQUIRE(batch->upload(target, 0, std::span<const std::byte>{}));

        // Submitting an empty batch yields the "no work" point, which reports itself complete.
        const auto nothing = batch->submit();
        CT_REQUIRE(nothing);
        CT_REQUIRE(!nothing->valid());
        CT_REQUIRE(nothing->is_complete());

        // Discarding an unsubmitted batch returns its staging space; the destructor does it too.
        CT_REQUIRE(batch->upload(target, 0, as_bytes(pattern(16, 4))));
        batch->discard();
        CT_REQUIRE(!batch->valid());

        destroy_buffer(target);
        destroy_device(dev);
    }
} // namespace

int main()
{
    const std::pair<const char *, void (*)()> cases[] = {
        {"staging info", test_staging_info},
        {"batch uploads and reads back", test_batch_uploads_and_reads_back},
        {"many uploads in one submission", test_many_uploads_in_one_submission},
        {"write_buffer is ordered without help", test_write_buffer_is_ordered_without_help},
        {"initial data is asynchronous", test_initial_data_is_asynchronous},
        {"one-shot upload", test_one_shot_upload},
        {"staging exhaustion", test_staging_exhaustion},
        {"ring wraps", test_ring_wraps},
        {"readback states", test_readback_states},
        {"co_await readback", test_co_await_readback},
        {"dropping a parked download", test_dropping_a_parked_download_does_not_strand_it},
        {"invalid arguments", test_invalid_arguments},
    };

    for (const auto &[name, fn] : cases)
    {
        std::printf("  %s ...\n", name);
        std::fflush(stdout);
        fn();
    }

    std::printf("rendering transfers: ok (backend %s)\n", to_string(backend()));
    return 0;
}
