/**
 * @file test_scan.cpp
 * @brief Differential tests for catalyst::text::scan: the SWAR fast path must agree with the scalar
 * reference on every input, for every stop set a caller might instantiate it with.
 * @details The JSON suite already pins JSON's own stop set end-to-end. What this adds is coverage of
 * the generalisation itself -- that the mask folding is right for one stop byte and for four, with
 * control bytes stopping the scan and with them treated as content -- so a new format can pick a stop
 * set with some confidence that the scanner will behave.
 * License: MIT (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/text/scan.hpp>

#include <cstdint>
#include <string>
#include <string_view>

using namespace catalyst::text::scan;

namespace
{
    /// A small deterministic PRNG, so a failure is reproducible from the source alone.
    class rng
    {
    public:
        std::uint32_t operator()() noexcept
        {
            state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<std::uint32_t>(state_ >> 33);
        }

    private:
        std::uint64_t state_ = 0x9E3779B97F4A7C15ULL;
    };

    /**
     * @brief Assert swar == scalar for one stop set over random strings and every start offset.
     * @details The alphabet is weighted towards ordinary letters so that runs are long enough to
     * exercise the 8-byte loop, but stop bytes, control bytes and high bytes all appear often enough
     * to land in the tail and at word boundaries.
     */
    template <control_bytes Controls, unsigned char... Stops>
    void differential(const char *label)
    {
        constexpr unsigned char stops[] = {Stops...};
        rng next;

        for (int iter = 0; iter < 4000; ++iter)
        {
            const std::size_t len = next() % 64;
            std::string s;
            for (std::size_t i = 0; i < len; ++i)
            {
                char c;
                switch (next() % 24)
                {
                case 0:
                case 1:
                    c = static_cast<char>(stops[next() % (sizeof...(Stops))]);
                    break;
                case 2:
                    c = static_cast<char>(next() % 0x20); // control byte
                    break;
                case 3:
                    c = static_cast<char>(0x80 + next() % 0x80); // UTF-8 byte: never a stop
                    break;
                case 4:
                    c = static_cast<char>(0x7F); // DEL: not a control byte
                    break;
                default:
                    c = static_cast<char>('a' + next() % 26);
                    break;
                }
                s.push_back(c);
            }

            for (std::size_t from = 0; from <= len; ++from)
            {
                if (swar<Controls, Stops...>(s, from) != scalar<Controls, Stops...>(s, from))
                {
                    std::cerr << "scan mismatch in " << label << " at from=" << from << "\n";
                    CT_REQUIRE(false);
                }
            }
        }

        // Each stop byte, at every position of a run long enough to cross the word loop into the tail.
        for (unsigned char stop : stops)
        {
            for (std::size_t pos = 0; pos < 41; ++pos)
            {
                std::string s(41, 'a');
                s[pos] = static_cast<char>(stop);
                CT_REQUIRE((swar<Controls, Stops...>(s, 0)) == pos);
                CT_REQUIRE((scalar<Controls, Stops...>(s, 0)) == pos);
            }
        }

        // Degenerate inputs: no stop at all, nothing to scan, and a start at the very end.
        CT_REQUIRE((swar<Controls, Stops...>(std::string(64, 'z'), 0)) == 64);
        CT_REQUIRE((scalar<Controls, Stops...>(std::string(64, 'z'), 0)) == 64);
        CT_REQUIRE((swar<Controls, Stops...>("", 0)) == 0);
        CT_REQUIRE((swar<Controls, Stops...>("abc", 3)) == 3);
    }

    void test_json_stop_set()
    {
        differential<control_bytes::stop, '"', '\\'>("json");
    }

    void test_csv_stop_set()
    {
        // The set a CSV field scan needs: control bytes are content, because CR and LF are legal
        // inside a quoted field and are listed explicitly instead.
        differential<control_bytes::allowed, ',', '"', '\r', '\n'>("csv");
    }

    void test_single_stop_byte()
    {
        differential<control_bytes::allowed, '\n'>("line");
    }

    /// Control bytes must stop the scan under `stop` and be invisible under `allowed`.
    void test_control_byte_policy()
    {
        const std::string s = "ab\tcd\"ef";

        CT_REQUIRE((swar<control_bytes::stop, '"'>(s, 0)) == 2);    // the tab stops it
        CT_REQUIRE((swar<control_bytes::allowed, '"'>(s, 0)) == 5); // the tab is content

        // 0x7F and bytes >= 0x80 are never control bytes for this purpose.
        const std::string high = "ab\x7F\xC3\xA9z|";
        CT_REQUIRE((swar<control_bytes::stop, '|'>(high, 0)) == 6);
    }

    /// A stop byte must be found no matter where it falls relative to the 8-byte word loop.
    void test_word_boundaries()
    {
        for (std::size_t len = 0; len < 40; ++len)
        {
            for (std::size_t pos = 0; pos < len; ++pos)
            {
                std::string s(len, 'x');
                s[pos] = ',';
                CT_REQUIRE((swar<control_bytes::allowed, ','>(s, 0)) == pos);
                CT_REQUIRE((scalar<control_bytes::allowed, ','>(s, 0)) == pos);
            }
        }
    }

    void test_whitespace()
    {
        CT_REQUIRE(skip_ws("   \t\r\nx", 0) == 6);
        CT_REQUIRE(skip_ws("x   ", 0) == 0);
        CT_REQUIRE(skip_ws("    ", 0) == 4);
        CT_REQUIRE(skip_ws("", 0) == 0);
        CT_REQUIRE(skip_ws("  a  b", 2) == 2); // already on a non-space: no movement
        CT_REQUIRE(skip_ws("  a  b", 3) == 5); // mid-string run
        CT_REQUIRE(skip_ws("  a  b", 6) == 6); // start at the end

        CT_REQUIRE(is_ws(' '));
        CT_REQUIRE(is_ws('\t'));
        CT_REQUIRE(is_ws('\n'));
        CT_REQUIRE(is_ws('\r'));
        CT_REQUIRE(!is_ws('\v')); // deliberately not whitespace: JSON does not allow it
        CT_REQUIRE(!is_ws('a'));
        CT_REQUIRE(!is_ws(0xA0)); // NBSP is not ASCII whitespace
    }

} // namespace

int main()
{
    test_json_stop_set();
    test_csv_stop_set();
    test_single_stop_byte();
    test_control_byte_policy();
    test_word_boundaries();
    test_whitespace();

    std::cout << "catalyst.text.scan: all tests passed\n";
    return 0;
}
