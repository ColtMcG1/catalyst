/**
 * @file audio_backend_null.cpp
 * @brief Backend that accepts every operation and produces no audio.
 * @details Used where the platform has no supported audio API, and by an application that wants the
 * audio module present but silent. It spawns no threads and never calls the renderer; for a silent
 * stream that still exercises the renderer, use `offline_stream` instead.
 *
 * Because everything it does not do comes from `backend_base` - stats, failure reporting, notices,
 * the render dispatcher - what is left is exactly the part that is about being silent.
 * License: MIT (see LICENSE).
 */

#include "../detail_backend.hpp"
#include "../detail_backend_base.hpp"
#include "../detail_render.hpp"

namespace catalyst::audio::detail
{

    namespace
    {

        class null_backend final : public backend_base
        {
        public:
            explicit null_backend(open_request request)
                : backend_base(backend_kind::null, std::move(request))
            {
            }

            [[nodiscard]] std::expected<std::vector<device_info>, error> enumerate_devices() const override
            {
                return std::vector<device_info>{};
            }

            std::expected<void, error> open() override
            {
                identify("null", "Null");
                set_rate(request_.sample_rate);
                mark_opened();
                return {};
            }

            std::expected<void, error> start() override
            {
                running_.store(true, std::memory_order_release);
                return {};
            }

            void stop() noexcept override { running_.store(false, std::memory_order_release); }

            void close() noexcept override
            {
                stop();
                mark_closed();
            }

            [[nodiscard]] stream_info info() const override
            {
                stream_info out = base_info();
                out.output_channels = request_.output_channels;
                out.input_channels = request_.input_channels;
                out.output_layout = layout_for(request_.output_channels);
                out.block_frames = request_.block_frames;
                out.output_latency = frames_to_time(request_.block_frames, request_.sample_rate);

                // `info()` is answerable before `open()`, and a caller that asks then should still
                // be told which device this would be rather than an empty name.
                if (out.device_id.empty())
                {
                    out.device_id = "null";
                    out.device_name = "Null";
                    out.sample_rate = request_.sample_rate;
                }

                return out;
            }
        };

    } // namespace

    std::unique_ptr<backend> create_null_backend(const open_request &request)
    {
        return std::make_unique<null_backend>(request);
    }

} // namespace catalyst::audio::detail
