/**
 * @file test_events.cpp
 * @brief Exercises the seam between a stream and a `catalyst::events::bus`: that events reach
 * listeners from `pump()` and not before, that a stream with no bus costs nothing, and that the
 * module's tags are where they are documented to be.
 * @details Runs against the null backend, so no hardware and no timing is involved. What cannot be
 * tested here is a real device disappearing; the queue that carries that is the same one these
 * events travel on, and `stream::pump()` is the only thing that drains it.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/audio/audio.hpp>
#include <catalyst/events/bus.hpp>

#include <vector>

using namespace catalyst;
using namespace catalyst::audio;

namespace
{

    stream_config null_config(events::bus *bus)
    {
        stream_config config;
        config.backend = backend_kind::null;
        config.sample_rate = 44100;
        config.output_channels = 2;
        config.block_frames = 128;
        config.bus = bus;
        return config;
    }

    /// The tag block is a published fact - a serialised filter may name one - so it is asserted
    /// rather than left to drift.
    void test_tags_are_stable()
    {
        CT_REQUIRE(tags::audio_base == 0x0002'0000u);

        // Distinct from the input module's block, which is the one immediately below.
        CT_REQUIRE(tags::audio_base > 0x0001'FFFFu);

        CT_REQUIRE(tags::device_added == tags::audio_base + 0x00);
        CT_REQUIRE(tags::device_removed == tags::audio_base + 0x01);
        CT_REQUIRE(tags::default_device_changed == tags::audio_base + 0x02);
        CT_REQUIRE(tags::device_lost == tags::audio_base + 0x03);
        CT_REQUIRE(tags::stream_started == tags::audio_base + 0x10);
        CT_REQUIRE(tags::stream_stopped == tags::audio_base + 0x11);
        CT_REQUIRE(tags::stream_failed == tags::audio_base + 0x12);
        CT_REQUIRE(tags::xrun == tags::audio_base + 0x13);

        // Every event carries its tag statically, so the bus dispatches on a constant.
        CT_REQUIRE(device_added_event::tag == tags::device_added);
        CT_REQUIRE(stream_started_event::tag == tags::stream_started);
        CT_REQUIRE(xrun_event::tag == tags::xrun);
    }

    /// Nothing is published from `start()` itself: events arrive on the caller's thread, inside
    /// `pump()`, which is the entire reason the queue exists.
    void test_events_arrive_from_pump()
    {
        events::bus bus;

        std::vector<stream_started_event> started;
        std::vector<stream_stopped_event> stopped;

        auto started_token = bus.add_listener<stream_started_event>(
            [&started](const stream_started_event &event) { started.push_back(event); });
        auto stopped_token = bus.add_listener<stream_stopped_event>(
            [&stopped](const stream_stopped_event &event) { stopped.push_back(event); });

        auto opened = stream::open(null_config(&bus), renderer{});
        CT_REQUIRE(opened.has_value());

        // Opening publishes nothing, and neither does starting - until it is pumped.
        opened->pump();
        CT_REQUIRE(started.empty());

        CT_REQUIRE(opened->start().has_value());
        CT_REQUIRE(started.empty());

        opened->pump();
        CT_REQUIRE(started.size() == 1);
        CT_REQUIRE(started[0].backend == backend_kind::null);
        CT_REQUIRE(started[0].sample_rate == 44100);
        CT_REQUIRE(started[0].output_channels == 2);
        CT_REQUIRE(started[0].input_channels == 0);

        // Reported once, not once per pump.
        opened->pump();
        CT_REQUIRE(started.size() == 1);

        opened->stop();
        CT_REQUIRE(stopped.empty());

        opened->pump();
        CT_REQUIRE(stopped.size() == 1);
        CT_REQUIRE(stopped[0].backend == backend_kind::null);

        opened->pump();
        CT_REQUIRE(stopped.size() == 1);

        started_token.remove();
        stopped_token.remove();
    }

    /// A stream given no bus publishes nothing and must not mind being pumped anyway.
    void test_no_bus_is_free()
    {
        auto opened = stream::open(null_config(nullptr), renderer{});
        CT_REQUIRE(opened.has_value());

        opened->pump();
        CT_REQUIRE(opened->start().has_value());
        opened->pump();
        opened->stop();
        opened->pump();

        CT_REQUIRE(!opened->is_running());
    }

    /// The bus outlives the stream, and a stream that has been moved from is still safe to pump.
    void test_pump_survives_a_move()
    {
        events::bus bus;

        int starts = 0;
        auto token = bus.add_listener<stream_started_event>(
            [&starts](const stream_started_event &) { ++starts; });

        auto opened = stream::open(null_config(&bus), renderer{});
        CT_REQUIRE(opened.has_value());
        CT_REQUIRE(opened->start().has_value());

        stream moved = std::move(*opened);

        // The moved-from stream has nothing to say; the moved-to one still owes a start event.
        opened->pump();
        CT_REQUIRE(starts == 0);

        moved.pump();
        CT_REQUIRE(starts == 1);

        token.remove();
    }

} // namespace

int main()
{
    test_tags_are_stable();
    test_events_arrive_from_pump();
    test_no_bus_is_free();
    test_pump_survives_a_move();

    return 0;
}
