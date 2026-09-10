/**
 * @file audio_backend_null.cpp
 * @brief Backend that accepts every operation and produces no audio.
 * @details Used where the platform has no supported audio API, and by an application that wants the
 * audio module present but silent. It spawns no threads and never calls the renderer; for a silent
 * stream that still exercises the renderer, use `offline_stream` instead.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../detail_backend.hpp"
#include "../detail_render.hpp"

namespace catalyst::audio::detail
{

    namespace
    {

        class null_backend final : public backend
        {
        public:
            explicit null_backend(open_request request) : request_(std::move(request)) {}

            [[nodiscard]] backend_kind kind() const noexcept override { return backend_kind::null; }

            [[nodiscard]] std::expected<std::vector<device_info>, error> enumerate_devices() const override
            {
                return std::vector<device_info>{};
            }

            std::expected<void, error> open() override
            {
                stats_.reset();
                return {};
            }

            std::expected<void, error> start() override
            {
                running_ = true;
                return {};
            }

            void stop() noexcept override { running_ = false; }

            void close() noexcept override { running_ = false; }

            [[nodiscard]] bool is_running() const noexcept override { return running_; }

            [[nodiscard]] stream_info info() const override
            {
                stream_info out;
                out.backend = backend_kind::null;
                out.direction = request_.direction;
                out.sample_rate = request_.sample_rate;
                out.output_channels = request_.output_channels;
                out.input_channels = request_.input_channels;
                out.output_layout = layout_for(request_.output_channels);
                out.block_frames = request_.block_frames;
                out.output_latency = frames_to_time(request_.block_frames, request_.sample_rate);
                out.device_id = "null";
                out.device_name = "Null";
                return out;
            }

            [[nodiscard]] stream_stats stats() const noexcept override { return stats_.snapshot(); }
            void reset_stats() noexcept override { stats_.reset(); }

        private:
            open_request request_{};
            stats_block stats_;
            bool running_ = false;
        };

    } // namespace

    std::unique_ptr<backend> create_null_backend(const open_request &request)
    {
        return std::make_unique<null_backend>(request);
    }

} // namespace catalyst::audio::detail
