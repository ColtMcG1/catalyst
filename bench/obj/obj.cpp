/**
 * @file obj.cpp
 * @brief Throughput benchmarks for the catalyst::resource::obj parser, plus a head-to-head on the
 * tokenizer that sits inside its inner loop.
 * @details Two things are measured here, and they answer different questions.
 *
 * The first is end-to-end: generate deterministic OBJ text of a few shapes an exporter actually
 * emits -- triangles with full `v/vt/vn` corners, positions-only triangles, `v//vn` quads, and a
 * file padded with the group/material/smoothing records this parser skips -- and report how fast
 * @ref parser::parse turns each into an @ref obj. That is the number that matters to a caller, and
 * it is the one against which any change to the parser has to justify itself.
 *
 * The second is a microbenchmark of `next_token` alone. The parser's tokenizer is a private
 * implementation detail, so three candidate implementations are reproduced here and run over the
 * same bytes, in the same line-splitting loop, interleaved in one process. Reproducing them means
 * they can drift from the real one, so @ref check_agreement pins all three against each other on
 * every payload before anything is timed. Isolating them this way is the point: a full-parse
 * comparison across two builds cannot be interleaved and has to fight machine noise, while these
 * three run microseconds apart on the same hot cache.
 *
 * Every timing is the best of several interleaved repetitions rather than a mean. On a laptop the
 * distribution is bounded below by the real cost and has a long tail of interference, so the
 * minimum is the stable statistic; the spread is printed alongside so a run whose tail swamped the
 * signal is visible rather than silently reported as a result.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/resource/obj/obj.hpp>
#include <catalyst/resource/obj/parser.hpp>
#include <catalyst/text/scan.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace obj = catalyst::resource::obj;
namespace scan = catalyst::text::scan;

namespace
{
    // ---------------------------------------------------------------------------------------------
    // Configuration
    // ---------------------------------------------------------------------------------------------

    constexpr std::size_t target_vertices = 40'000; ///< Grid size; sets each payload to a few MB.
    constexpr std::size_t iterations = 5;           ///< Timed passes inside one repetition.
    constexpr std::size_t repetitions = 7;          ///< Interleaved repetitions; the best one wins.

    // ---------------------------------------------------------------------------------------------
    // The tokenizer, three ways
    //
    // All three have identical semantics: take the next separator-delimited token off the front of
    // `rest`, advance `rest` to the separator that ended it, and return an empty view -- emptying
    // `rest` -- once the line is exhausted. A token is never empty, so "empty" means "no more".
    // ---------------------------------------------------------------------------------------------

    /// @brief The bytes that separate OBJ fields. CR is here so a CRLF file needs no pre-pass.
    constexpr std::string_view field_separators = " \t\r";

    /// @brief Whether @p c is one of @ref field_separators. Mirrors the stop set given to the scans.
    [[nodiscard]] constexpr bool is_separator(unsigned char c) noexcept
    {
        return c == ' ' || c == '\t' || c == '\r';
    }

    /// @brief Baseline: the `find_first_not_of` / `find_first_of` pair the parser ships today.
    [[nodiscard]] std::string_view next_token_std(std::string_view &rest) noexcept
    {
        const auto start = rest.find_first_not_of(field_separators);
        if (start == std::string_view::npos)
        {
            rest = {};
            return {};
        }

        const auto stop = rest.find_first_of(field_separators, start);
        const std::string_view token = rest.substr(start, stop - start);
        rest = (stop == std::string_view::npos) ? std::string_view{} : rest.substr(stop);
        return token;
    }

    /// @brief The same two scans written out by hand, so the separator set is three compares the
    /// compiler can see rather than a library call over a runtime-length set. Separates the cost of
    /// escaping `find_first_of` from the cost of the word-at-a-time trick.
    [[nodiscard]] std::string_view next_token_scalar(std::string_view &rest) noexcept
    {
        const std::size_t n = rest.size();

        std::size_t start = 0;
        while (start < n && is_separator(static_cast<unsigned char>(rest[start])))
            ++start;
        if (start == n)
        {
            rest = {};
            return {};
        }

        std::size_t stop = start;
        while (stop < n && !is_separator(static_cast<unsigned char>(rest[stop])))
            ++stop;

        const std::string_view token = rest.substr(start, stop - start);
        rest = (stop == n) ? std::string_view{} : rest.substr(stop);
        return token;
    }

    /// @brief The token body scanned eight bytes at a time via `catalyst::text::scan::swar`.
    ///
    /// Only the body scan is batched. The leading separator run is left scalar for the same reason
    /// `scan::skip_ws` is: between two OBJ columns it is almost always exactly one byte, and the
    /// fixed setup a SWAR word costs is never amortized over a run that short.
    [[nodiscard]] std::string_view next_token_swar(std::string_view &rest) noexcept
    {
        const std::size_t n = rest.size();

        std::size_t start = 0;
        while (start < n && is_separator(static_cast<unsigned char>(rest[start])))
            ++start;
        if (start == n)
        {
            rest = {};
            return {};
        }

        const std::size_t stop = scan::swar<scan::control_bytes::allowed, ' ', '\t', '\r'>(rest, start);

        const std::string_view token = rest.substr(start, stop - start);
        rest = (stop == n) ? std::string_view{} : rest.substr(stop);
        return token;
    }

    // ---------------------------------------------------------------------------------------------
    // Deterministic generation
    // ---------------------------------------------------------------------------------------------

    /// @brief Small LCG, so every payload is byte-identical across runs and machines.
    class rng
    {
    public:
        explicit rng(std::uint64_t seed) noexcept : state_(seed) {}

        std::uint64_t next() noexcept
        {
            state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
            return state_ >> 33;
        }

        std::uint64_t below(std::uint64_t n) noexcept { return next() % n; }

        /// @brief A coordinate with the digit count a real exporter writes.
        double coordinate() noexcept
        {
            return (static_cast<double>(below(2'000'000)) - 1'000'000.0) / 1'000.0;
        }

    private:
        std::uint64_t state_;
    };

    void append_formatted(std::string &out, const char *format, double a, double b, double c)
    {
        char buffer[96];
        const int written = std::snprintf(buffer, sizeof buffer, format, a, b, c);
        out.append(buffer, static_cast<std::size_t>(written));
    }

    void append_formatted(std::string &out, const char *format, double a, double b)
    {
        char buffer[96];
        const int written = std::snprintf(buffer, sizeof buffer, format, a, b);
        out.append(buffer, static_cast<std::size_t>(written));
    }

    /// @brief What a payload is and what it is worth per iteration.
    struct payload
    {
        std::string name;
        std::string text;
        std::size_t lines = 0;   ///< Newline-delimited records, including the skipped ones.
        std::size_t tokens = 0;  ///< Tokens the parser's inner loop will pull out. Filled in later.
    };

    /// @brief How a payload spells its face corners, which is most of what varies between exporters.
    enum class corner_form
    {
        position_only, ///< `f 1 2 3`
        position_normal, ///< `f 1//4 2//5 3//6`
        full ///< `f 1/1/1 2/2/2 3/3/3`
    };

    struct recipe
    {
        std::string name;
        corner_form corners = corner_form::full;
        std::size_t corners_per_face = 3; ///< 3 for triangles, 4 for the quad payload.
        bool annotate = false;            ///< Emit the `g`/`o`/`s`/`usemtl` records this parser skips.
        bool crlf = false;                ///< Terminate lines with CRLF, so every line ends in a CR.
    };

    /**
     * @fn generate
     * @brief Build one payload: the vertex blocks an exporter writes first, then the faces.
     *
     * Faces index a moving window of recently declared vertices rather than the whole file, which is
     * what a real mesh does and what keeps `resolve_index`'s bounds checks realistic.
     */
    payload generate(const recipe &r)
    {
        payload p;
        p.name = r.name;
        p.text.reserve(target_vertices * 160);
        rng random(0x9E3779B97F4A7C15ull);

        const std::string_view eol = r.crlf ? "\r\n" : "\n";
        const bool wants_texcoords = r.corners == corner_form::full;
        const bool wants_normals = r.corners != corner_form::position_only;

        const auto line = [&](std::string_view content) {
            p.text += content;
            p.text += eol;
            ++p.lines;
        };

        line("# generated by catalyst_bench_obj");
        line("mtllib bench.mtl");

        for (std::size_t i = 0; i < target_vertices; ++i)
        {
            append_formatted(p.text, "v %.6f %.6f %.6f", random.coordinate(), random.coordinate(),
                             random.coordinate());
            p.text += eol;
            ++p.lines;
        }

        if (wants_texcoords)
            for (std::size_t i = 0; i < target_vertices; ++i)
            {
                append_formatted(p.text, "vt %.6f %.6f",
                                 static_cast<double>(random.below(1'000'000)) / 1'000'000.0,
                                 static_cast<double>(random.below(1'000'000)) / 1'000'000.0);
                p.text += eol;
                ++p.lines;
            }

        if (wants_normals)
            for (std::size_t i = 0; i < target_vertices; ++i)
            {
                append_formatted(p.text, "vn %.4f %.4f %.4f",
                                 static_cast<double>(random.below(20'000)) / 10'000.0 - 1.0,
                                 static_cast<double>(random.below(20'000)) / 10'000.0 - 1.0,
                                 static_cast<double>(random.below(20'000)) / 10'000.0 - 1.0);
                p.text += eol;
                ++p.lines;
            }

        // Two faces per vertex is roughly what a closed triangle mesh has; the quad payload gets
        // half as many so the two stay comparable in corner count.
        const std::size_t face_count = r.corners_per_face == 3 ? target_vertices * 2 : target_vertices;
        const std::size_t window = 64; ///< Faces index within this many recently declared vertices.

        std::string face;
        for (std::size_t f = 0; f < face_count; ++f)
        {
            if (r.annotate && f % 512 == 0)
            {
                line("g piece_" + std::to_string(f / 512));
                line("usemtl material_" + std::to_string((f / 512) % 8));
                line("s " + std::to_string((f / 512) % 2));
                line("# a comment an exporter left behind");
            }

            // Slide the window forward so the whole vertex array is touched, not just its head.
            const std::size_t base = 1 + (f * target_vertices / face_count);
            face = "f";
            for (std::size_t c = 0; c < r.corners_per_face; ++c)
            {
                std::size_t index = base + random.below(window);
                if (index > target_vertices)
                    index = target_vertices;

                face += ' ';
                const std::string digits = std::to_string(index);
                switch (r.corners)
                {
                case corner_form::position_only:
                    face += digits;
                    break;
                case corner_form::position_normal:
                    face += digits;
                    face += "//";
                    face += digits;
                    break;
                case corner_form::full:
                    face += digits;
                    face += '/';
                    face += digits;
                    face += '/';
                    face += digits;
                    break;
                }
            }
            line(face);
        }

        return p;
    }

    std::vector<payload> make_payloads()
    {
        std::vector<payload> out;
        out.push_back(generate({.name = "triangles v/vt/vn", .corners = corner_form::full}));
        out.push_back(generate({.name = "triangles v only", .corners = corner_form::position_only}));
        out.push_back(generate({.name = "quads v//vn",
                                .corners = corner_form::position_normal,
                                .corners_per_face = 4}));
        out.push_back(generate({.name = "annotated + CRLF",
                                .corners = corner_form::full,
                                .annotate = true,
                                .crlf = true}));
        return out;
    }

    // ---------------------------------------------------------------------------------------------
    // Tokenizing
    // ---------------------------------------------------------------------------------------------

    /**
     * @fn tokenize
     * @brief Walk @p text exactly the way `parser::parse` does, but do nothing with the tokens.
     *
     * The line splitting and the comment trim are copied from the parser so the tokenizer is
     * measured under the call pattern it really sees: many short lines, a fresh `rest` per line, and
     * a loop that ends on the empty token rather than on a count.
     *
     * @return The summed token lengths, which is both a cheap checksum and enough of a data
     * dependency that no part of the walk can be optimized away.
     */
    template <class Tokenizer>
    std::size_t tokenize(const std::string &text, Tokenizer &&next)
    {
        const std::string_view all(text);
        std::size_t checksum = 0;

        for (const auto chunk : std::views::split(all, '\n'))
        {
            std::string_view line(chunk.begin(), chunk.end());

            if (const auto hash = line.find('#'); hash != std::string_view::npos)
                line = line.substr(0, hash);

            for (;;)
            {
                const std::string_view token = next(line);
                if (token.empty())
                    break;
                checksum += token.size();
            }
        }

        return checksum;
    }

    std::size_t tokenize_std(const std::string &text) { return tokenize(text, next_token_std); }
    std::size_t tokenize_scalar(const std::string &text) { return tokenize(text, next_token_scalar); }
    std::size_t tokenize_swar(const std::string &text) { return tokenize(text, next_token_swar); }

    /**
     * @fn check_agreement
     * @brief Assert the three tokenizers produce the identical token sequence on @p p.
     *
     * The three are reproduced from the parser rather than shared with it, so this is the only thing
     * standing between a divergence and a benchmark that cheerfully times the wrong answer. Compares
     * the tokens themselves, not just the checksum: two different splits can sum to the same length.
     */
    /**
     * @brief Lines the generators do not produce, where the three tokenizers could plausibly differ.
     *
     * The generated payloads are well-formed exporter output: one space between columns, no leading
     * indentation, no tabs. Every interesting disagreement lives outside that -- a run of separators
     * rather than one, a token that ends exactly on a SWAR word boundary, a line that is nothing but
     * separators, a bare CR. They are cheap to check and they are the whole risk of the change.
     */
    constexpr std::string_view adversarial_lines[] = {
        "",
        " ",
        "\t",
        "\r",
        "   \t \r",
        "v 1 2 3",
        "  v   1.0   2.0   3.0  ",
        "\tv\t1.0\t2.0\t3.0\t",
        "v 1.0 2.0 3.0\r",
        "f 1/1/1 2/2/2 3/3/3",
        "f 1//1 2//2 3//3",
        "12345678 1234567 123456789", // Tokens either side of the 8-byte word the SWAR scan reads.
        "1234567812345678 a",         // Exactly two words, then a short one.
        "a 1234567",
        "usemtl some_material_name",
        "vn -0.0000 1.0000 -0.0000",
    };

    bool check_agreement(const payload &p)
    {
        // The hand-written cases first: a failure there is far easier to read than one buried in a
        // few million generated lines.
        for (const std::string_view probe : adversarial_lines)
        {
            std::string_view a = probe;
            std::string_view b = probe;
            std::string_view c = probe;
            for (;;)
            {
                const std::string_view ta = next_token_std(a);
                const std::string_view tb = next_token_scalar(b);
                const std::string_view tc = next_token_swar(c);
                if (ta != tb || ta != tc)
                {
                    std::cerr << "tokenizers disagree on '" << probe << "': std='" << ta
                              << "' scalar='" << tb << "' swar='" << tc << "'\n";
                    return false;
                }
                if (ta.empty())
                    break;
            }
        }

        const std::string_view all(p.text);
        std::size_t line_number = 0;

        for (const auto chunk : std::views::split(all, '\n'))
        {
            ++line_number;
            std::string_view line(chunk.begin(), chunk.end());
            if (const auto hash = line.find('#'); hash != std::string_view::npos)
                line = line.substr(0, hash);

            std::string_view a = line;
            std::string_view b = line;
            std::string_view c = line;

            for (;;)
            {
                const std::string_view ta = next_token_std(a);
                const std::string_view tb = next_token_scalar(b);
                const std::string_view tc = next_token_swar(c);

                if (ta != tb || ta != tc)
                {
                    std::cerr << p.name << ": tokenizers disagree on line " << line_number << ": std='"
                              << ta << "' scalar='" << tb << "' swar='" << tc << "'\n";
                    return false;
                }
                if (ta.empty())
                    break;
            }
        }

        return true;
    }

    // ---------------------------------------------------------------------------------------------
    // Timing
    // ---------------------------------------------------------------------------------------------

    using clock = std::chrono::steady_clock;

    /// @brief The best and worst of several repetitions of the same measurement.
    struct sample
    {
        double best = 0.0;  ///< Seconds for the fastest repetition. The reported figure.
        double worst = 0.0; ///< Seconds for the slowest. Only printed, as a noise indicator.

        /// @brief How far the slowest repetition ran above the fastest, as a percentage.
        [[nodiscard]] double spread_percent() const noexcept
        {
            return best > 0.0 ? (worst / best - 1.0) * 100.0 : 0.0;
        }
    };

    /// @brief Accumulates repetitions of one measurement.
    class collector
    {
    public:
        void add(double seconds) noexcept
        {
            if (count_ == 0 || seconds < s_.best)
                s_.best = seconds;
            if (seconds > s_.worst)
                s_.worst = seconds;
            ++count_;
        }

        [[nodiscard]] const sample &result() const noexcept { return s_; }

    private:
        sample s_;
        std::size_t count_ = 0;
    };

    /// @brief One timed repetition: @ref iterations passes, folding into @p checksum so nothing is
    /// elided.
    template <class Operation>
    double time_once(Operation &&operation, std::size_t &checksum)
    {
        const auto start = clock::now();
        for (std::size_t i = 0; i < iterations; ++i)
            checksum += std::invoke(operation);
        return std::chrono::duration<double>(clock::now() - start).count();
    }

    double megabytes_per_second(const payload &p, double seconds)
    {
        return static_cast<double>(p.text.size()) * static_cast<double>(iterations) / seconds / 1e6;
    }

} // namespace

int main()
{
    std::cout << "catalyst::resource::obj throughput\n"
              << "  " << target_vertices << " vertices per payload, " << iterations
              << " passes per repetition, best of " << repetitions << " interleaved repetitions\n\n";

    auto payloads = make_payloads();

    // ------------------------------------------------------------------------------------------
    // Sanity: every payload must parse, and the three tokenizers must agree about it.
    // ------------------------------------------------------------------------------------------
    for (auto &p : payloads)
    {
        auto parsed = obj::parser::parse(p.text, p.name);
        if (!parsed)
        {
            std::cerr << p.name << ": generated payload failed to parse: " << parsed.error().message()
                      << '\n';
            return 1;
        }
        if (!check_agreement(p))
            return 1;

        // Count the tokens once, so the payload summary says how much work each line really is.
        std::size_t count = 0;
        const std::string_view all(p.text);
        for (const auto chunk : std::views::split(all, '\n'))
        {
            std::string_view line(chunk.begin(), chunk.end());
            if (const auto hash = line.find('#'); hash != std::string_view::npos)
                line = line.substr(0, hash);
            while (!next_token_std(line).empty())
                ++count;
        }
        p.tokens = count;

        std::cout << std::left << std::setw(22) << p.name << std::right << std::fixed
                  << std::setprecision(2) << std::setw(8)
                  << static_cast<double>(p.text.size()) / (1024.0 * 1024.0) << " MiB"
                  << std::setw(10) << p.lines << " lines" << std::setw(11) << p.tokens << " tokens"
                  << std::setw(10) << parsed->vertices.size() << " v" << std::setw(10)
                  << parsed->face_count() << " f\n";
    }
    std::cout << '\n';

    // ------------------------------------------------------------------------------------------
    // Full parse
    // ------------------------------------------------------------------------------------------
    std::vector<collector> parse_times(payloads.size());
    std::size_t checksum = 0;

    for (std::size_t rep = 0; rep < repetitions; ++rep)
        for (std::size_t i = 0; i < payloads.size(); ++i)
            parse_times[i].add(time_once(
                [&] {
                    auto r = obj::parser::parse(payloads[i].text, payloads[i].name);
                    return r ? r->face_vertices.size() : 0u;
                },
                checksum));

    std::cout << "parser::parse (end to end)\n"
              << "  " << std::left << std::setw(22) << "payload" << std::right << std::setw(10)
              << "MB/s" << std::setw(12) << "ms/iter" << std::setw(10) << "spread" << '\n';
    for (std::size_t i = 0; i < payloads.size(); ++i)
    {
        const auto &s = parse_times[i].result();
        std::cout << "  " << std::left << std::setw(22) << payloads[i].name << std::right << std::fixed
                  << std::setprecision(1) << std::setw(10) << megabytes_per_second(payloads[i], s.best)
                  << std::setprecision(3) << std::setw(12)
                  << s.best * 1'000.0 / static_cast<double>(iterations) << std::setprecision(1)
                  << std::setw(9) << s.spread_percent() << "%\n";
    }
    std::cout << '\n';

    // ------------------------------------------------------------------------------------------
    // next_token alone, three implementations interleaved
    // ------------------------------------------------------------------------------------------
    struct variant
    {
        const char *name;
        std::size_t (*run)(const std::string &);
    };
    constexpr variant variants[] = {
        {"std", &tokenize_std},
        {"scalar", &tokenize_scalar},
        {"swar", &tokenize_swar},
    };
    constexpr std::size_t variant_count = std::size(variants);

    std::vector<collector> token_times(payloads.size() * variant_count);

    for (std::size_t rep = 0; rep < repetitions; ++rep)
        for (std::size_t i = 0; i < payloads.size(); ++i)
            for (std::size_t v = 0; v < variant_count; ++v)
                token_times[i * variant_count + v].add(
                    time_once([&] { return variants[v].run(payloads[i].text); }, checksum));

    std::cout << "next_token only (same line splitting, tokens discarded)\n"
              << "  " << std::left << std::setw(22) << "payload" << std::right << std::setw(11)
              << "std MB/s" << std::setw(11) << "scalar" << std::setw(11) << "swar" << std::setw(15)
              << "scalar vs std" << std::setw(15) << "swar vs std" << std::setw(10) << "spread"
              << '\n';
    for (std::size_t i = 0; i < payloads.size(); ++i)
    {
        const double base = megabytes_per_second(payloads[i], token_times[i * variant_count].result().best);

        std::cout << "  " << std::left << std::setw(22) << payloads[i].name << std::right << std::fixed
                  << std::setprecision(1);
        for (std::size_t v = 0; v < variant_count; ++v)
            std::cout << std::setw(11)
                      << megabytes_per_second(payloads[i],
                                              token_times[i * variant_count + v].result().best);
        for (std::size_t v = 1; v < variant_count; ++v)
        {
            const double mbps = megabytes_per_second(payloads[i],
                                                     token_times[i * variant_count + v].result().best);
            std::cout << std::showpos << std::setw(14) << (mbps / base - 1.0) * 100.0 << std::noshowpos
                      << '%';
        }

        // The widest spread of the three, so a row whose comparison was measured through a stall is
        // visible rather than quietly believed.
        double spread = 0.0;
        for (std::size_t v = 0; v < variant_count; ++v)
            spread = std::max(spread, token_times[i * variant_count + v].result().spread_percent());
        std::cout << std::setw(9) << spread << "%\n";
    }

    std::cout << "\n  worst-case spread across repetitions: ";
    double worst = 0.0;
    for (const auto &c : token_times)
        worst = worst > c.result().spread_percent() ? worst : c.result().spread_percent();
    for (const auto &c : parse_times)
        worst = worst > c.result().spread_percent() ? worst : c.result().spread_percent();
    std::cout << std::fixed << std::setprecision(1) << worst << "%\n";

    std::cout << "  [" << std::hex << (checksum & 0xffff) << std::dec << "]\n";
    return 0;
}
