/**
 * @file error.cpp
 * @brief Renders an `error` as the sentence a log line wants.
 * License: MIT (see LICENSE).
 */

#include <catalyst/audio/error.hpp>

#include <string>

namespace catalyst::audio
{

    std::string error::message() const
    {
        std::string out;
        out.reserve(96);

        // The backend first, because "WASAPI could not do this" and "ASIO could not do this" are
        // different problems with different fixes, and the code alone does not say which happened.
        if (backend != backend_kind::automatic)
        {
            out += name(backend);
            out += ": ";
        }

        out += name(code);

        // Only worth printing when the backend actually found out what the device wanted; a zero
        // here means "not known", not "zero hertz".
        if (offered_sample_rate != 0 || offered_channels != 0)
        {
            out += " (device offers ";

            if (offered_sample_rate != 0)
            {
                out += std::to_string(offered_sample_rate);
                out += " Hz";
                if (offered_channels != 0)
                    out += ", ";
            }

            if (offered_channels != 0)
            {
                out += std::to_string(offered_channels);
                out += " ch";
            }

            out += ')';
        }

        return out;
    }

} // namespace catalyst::audio
