/**
 * @file json.cpp
 * @brief Throughput benchmarks for the catalyst::resource::json module.
 * @details Generates deterministic JSON payloads of roughly equal byte size whose scalars are all of one
 * kind (strings, escaped strings, integers, doubles) or a mix, then measures how fast each parses into the
 * owning `value` tree and the flat `document` tape, and how fast each serializes back out. Results are
 * reported in MB/s (bytes of JSON text per second) and Mval/s (scalar values per second), so the cost of
 * each scalar kind can be compared directly regardless of how many bytes it takes up.
 * License: MIT (see LICENSE).
 */

#include <benchmark.hpp>

#include <catalyst/resource/json/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace json = catalyst::resource::json;

namespace
{
    // -------------------------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------------------------

    constexpr std::size_t target_bytes = 1u << 20; ///< Approximate size of each generated payload.
    constexpr std::size_t row_width = 16;          ///< Scalars per array row / members per record.
    constexpr std::size_t iterations = 40;         ///< Timed passes per (payload, operation) pair.

    // -------------------------------------------------------------------------------------------
    // Deterministic generation
    // -------------------------------------------------------------------------------------------

    /// Small LCG so payloads are identical across runs and platforms.
    class rng
    {
    public:
        explicit rng(std::uint64_t seed) : state_(seed) {}

        std::uint64_t next() noexcept
        {
            state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
            return state_ >> 33;
        }

        std::uint64_t below(std::uint64_t n) noexcept { return next() % n; }

    private:
        std::uint64_t state_;
    };

    constexpr std::string_view words[] = {
        "alpha", "bravo",  "charlie", "delta", "echo",   "foxtrot", "golf",   "hotel",
        "india", "juliet", "kilo",    "lima",  "mike",   "november", "oscar", "papa",
        "quebec", "romeo", "sierra",  "tango", "uniform", "victor", "whiskey", "xray",
    };

    /// Plain ASCII string, no escapes: `"echo_1234_delta"`.
    void append_string(std::string &out, rng &r)
    {
        out += '"';
        out += words[r.below(std::size(words))];
        out += '_';
        out += std::to_string(r.below(100'000));
        out += '_';
        out += words[r.below(std::size(words))];
        out += '"';
    }

    /// String that exercises the escape decoder: quotes, backslashes, control escapes and \u sequences.
    void append_escaped_string(std::string &out, rng &r)
    {
        constexpr std::string_view escapes[] = {"\\n", "\\t", "\\\"", "\\\\", "\\/", "\\u00e9", "\\u4e2d", "\\r"};
        out += '"';
        out += words[r.below(std::size(words))];
        for (int i = 0; i < 3; ++i)
        {
            out += escapes[r.below(std::size(escapes))];
            out += words[r.below(std::size(words))];
        }
        out += '"';
    }

    /// Integer with a spread of magnitudes so both the short and near-19-digit paths are hit.
    void append_integer(std::string &out, rng &r)
    {
        const auto digits = 1 + r.below(18);
        std::uint64_t limit = 1;
        for (std::uint64_t i = 0; i < digits; ++i)
            limit *= 10;
        const auto magnitude = r.below(limit);
        if (r.below(4) == 0)
            out += '-';
        out += std::to_string(magnitude);
    }

    /// Double in a spread of forms: plain fractions, negatives, and exponent notation.
    void append_double(std::string &out, rng &r)
    {
        char buffer[64];
        const double mantissa = static_cast<double>(r.below(1'000'000'000)) / 1'000.0;
        const double sign = r.below(4) == 0 ? -1.0 : 1.0;
        int written = 0;
        switch (r.below(3))
        {
        case 0:
            written = std::snprintf(buffer, sizeof buffer, "%.3f", sign * mantissa);
            break;
        case 1:
            written = std::snprintf(buffer, sizeof buffer, "%.6f", sign * mantissa / 1'000.0);
            break;
        default:
            written = std::snprintf(buffer, sizeof buffer, "%.9e", sign * mantissa);
            break;
        }
        out.append(buffer, static_cast<std::size_t>(written));
    }

    /// One scalar of a rotating kind: string, integer, double, bool, null, escaped string.
    void append_mixed(std::string &out, rng &r, std::size_t index)
    {
        switch (index % 6)
        {
        case 0:
            append_string(out, r);
            break;
        case 1:
            append_integer(out, r);
            break;
        case 2:
            append_double(out, r);
            break;
        case 3:
            out += r.below(2) ? "true" : "false";
            break;
        case 4:
            out += "null";
            break;
        default:
            append_escaped_string(out, r);
            break;
        }
    }

    struct payload
    {
        std::string name;
        std::string text;
        std::size_t scalar_count = 0;
    };

    /// `[[s, s, ...], [s, s, ...], ...]`: rows of scalars with minimal structural overhead, so the scalar
    /// kind dominates the cost.
    template <class Emit>
    payload make_array_payload(std::string name, Emit &&emit)
    {
        payload p;
        p.name = std::move(name);
        p.text.reserve(target_bytes + 4096);
        rng r(0x9E3779B97F4A7C15ull);

        p.text += '[';
        bool first_row = true;
        while (p.text.size() < target_bytes)
        {
            if (!first_row)
                p.text += ',';
            first_row = false;
            p.text += '[';
            for (std::size_t i = 0; i < row_width; ++i)
            {
                if (i != 0)
                    p.text += ',';
                emit(p.text, r, p.scalar_count);
                ++p.scalar_count;
            }
            p.text += ']';
        }
        p.text += ']';
        return p;
    }

    /// `[{"k0": v, "k1": v, ...}, ...]`: the realistic shape, records with string keys and mixed values.
    payload make_record_payload()
    {
        payload p;
        p.name = "mixed records (objects, string keys)";
        p.text.reserve(target_bytes + 4096);
        rng r(0xD1B54A32D192ED03ull);

        std::vector<std::string> keys;
        for (std::size_t i = 0; i < row_width; ++i)
        {
            std::string key = "\"";
            key += words[i % std::size(words)];
            key += '_';
            key += std::to_string(i);
            key += "\":";
            keys.push_back(std::move(key));
        }

        p.text += '[';
        bool first_row = true;
        while (p.text.size() < target_bytes)
        {
            if (!first_row)
                p.text += ',';
            first_row = false;
            p.text += '{';
            for (std::size_t i = 0; i < row_width; ++i)
            {
                if (i != 0)
                    p.text += ',';
                p.text += keys[i];
                append_mixed(p.text, r, p.scalar_count);
                ++p.scalar_count;
            }
            p.text += '}';
        }
        p.text += ']';
        return p;
    }

    std::vector<payload> make_payloads()
    {
        std::vector<payload> out;
        out.push_back(make_array_payload("strings (plain ASCII)",
                                         [](std::string &t, rng &r, std::size_t) { append_string(t, r); }));
        out.push_back(make_array_payload("strings (escaped)",
                                         [](std::string &t, rng &r, std::size_t) { append_escaped_string(t, r); }));
        out.push_back(make_array_payload("integers",
                                         [](std::string &t, rng &r, std::size_t) { append_integer(t, r); }));
        out.push_back(make_array_payload("doubles",
                                         [](std::string &t, rng &r, std::size_t) { append_double(t, r); }));
        out.push_back(make_array_payload("mixed scalars (arrays)",
                                         [](std::string &t, rng &r, std::size_t i) { append_mixed(t, r, i); }));
        out.push_back(make_record_payload());
        return out;
    }

    // -------------------------------------------------------------------------------------------
    // Timing
    // -------------------------------------------------------------------------------------------

    /// Runs `operation` once untimed, then `iterations` times timed, and prints bytes/s and values/s.
    /// `operation` must return a `std::size_t` derived from its result so the work cannot be elided.
    template <class Operation>
    void run_throughput(std::string_view label, const payload &p, Operation &&operation)
    {
        using clock = std::chrono::steady_clock;

        std::size_t checksum = std::invoke(operation);

        const auto start = clock::now();
        for (std::size_t i = 0; i < iterations; ++i)
            checksum += std::invoke(operation);
        const auto elapsed = std::chrono::duration<double>(clock::now() - start).count();

        const double per_iteration_ms = elapsed * 1'000.0 / static_cast<double>(iterations);
        const double megabytes_per_second =
            static_cast<double>(p.text.size()) * static_cast<double>(iterations) / elapsed / 1e6;
        const double megavalues_per_second =
            static_cast<double>(p.scalar_count) * static_cast<double>(iterations) / elapsed / 1e6;

        std::cout << "  " << std::left << std::setw(18) << label << std::right << std::fixed
                  << std::setprecision(1) << std::setw(9) << megabytes_per_second << " MB/s" << std::setw(9)
                  << megavalues_per_second << " Mval/s" << std::setprecision(3) << std::setw(10)
                  << per_iteration_ms << " ms/iter"
                  << "   [" << std::hex << (checksum & 0xffff) << std::dec << "]\n";
    }

    /// Cheap traversal so the tape's cursor and the value tree are actually walked, not just built.
    std::size_t walk(const json::cursor &c)
    {
        if (c.is_array())
        {
            std::size_t n = 0;
            for (const auto &e : c.elements())
                n += walk(e);
            return n;
        }
        if (c.is_object())
        {
            std::size_t n = 0;
            for (const auto &m : c.members())
                n += walk(m.value);
            return n;
        }
        return 1;
    }

    std::size_t walk(const json::value &v)
    {
        if (v.is_array())
        {
            std::size_t n = 0;
            for (const auto &e : v.as_array())
                n += walk(e);
            return n;
        }
        if (v.is_object())
        {
            std::size_t n = 0;
            for (const auto &m : v.as_object())
                n += walk(m.second);
            return n;
        }
        return 1;
    }

    void bench_payload(const payload &p)
    {
        auto tree = json::parse(p.text);
        auto doc = json::parse_document(p.text);
        if (!tree || !doc)
        {
            std::cerr << p.name << ": generated payload failed to parse: "
                      << (!tree ? tree.error().message() : doc.error().message()) << '\n';
            return;
        }
        if (walk(*tree) != p.scalar_count || walk(doc->root()) != p.scalar_count)
        {
            std::cerr << p.name << ": scalar count mismatch after parse\n";
            return;
        }

        std::cout << p.name << '\n'
                  << "  bytes: " << p.text.size() << "   scalars: " << p.scalar_count
                  << "   bytes/scalar: " << std::fixed << std::setprecision(1)
                  << static_cast<double>(p.text.size()) / static_cast<double>(p.scalar_count) << '\n';

        run_throughput("parse -> value", p, [&] {
            auto r = json::parse(p.text);
            return r ? r->size() : 0u;
        });
        run_throughput("parse -> document", p, [&] {
            auto r = json::parse_document(p.text);
            return r ? r->root().size() : 0u;
        });
        run_throughput("walk value", p, [&] { return walk(*tree); });
        run_throughput("walk document", p, [&] { return walk(doc->root()); });
        run_throughput("dump value", p, [&] { return json::dump(*tree).size(); });
        run_throughput("dump document", p, [&] { return json::dump(*doc).size(); });
        std::cout << '\n';
    }

} // namespace

int main()
{
    std::cout << "catalyst::resource::json throughput\n"
              << "  payload target: " << target_bytes << " bytes, " << row_width << " scalars per row, "
              << iterations << " timed iterations per line\n\n";

    for (const auto &p : make_payloads())
        bench_payload(p);

    return 0;
}
