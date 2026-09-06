#include "../core/test_common.hpp"

#include <catalyst/resource/json/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace catalyst::resource::json;

namespace
{
    value must_parse(std::string_view text)
    {
        auto r = parse(text);
        if (!r)
            std::cerr << "unexpected parse failure on <" << text << ">: " << r.error().message() << "\n";
        CT_REQUIRE(r.has_value());
        return std::move(*r);
    }

    parse_error_code must_fail(std::string_view text)
    {
        auto r = parse(text);
        if (r)
            std::cerr << "unexpected parse success on <" << text << ">\n";
        CT_REQUIRE(!r.has_value());
        return r.error().code;
    }

    template <class Exception, class Fn>
    bool throws(Fn &&fn)
    {
        try
        {
            fn();
        }
        catch (const Exception &)
        {
            return true;
        }
        catch (...)
        {
            return false;
        }
        return false;
    }

    // -------------------------------------------------------------------------------------------
    // Scalars and numbers
    // -------------------------------------------------------------------------------------------

    void test_scalars()
    {
        CT_REQUIRE(must_parse("null").is_null());
        CT_REQUIRE(must_parse("true").as_bool() == true);
        CT_REQUIRE(must_parse("false").as_bool() == false);
        CT_REQUIRE(must_parse("42").as_int() == 42);
        CT_REQUIRE(must_parse("-42").as_int() == -42);
        CT_REQUIRE(must_parse("0").as_int() == 0);
        CT_REQUIRE(must_parse("-0").as_int() == 0);
        CT_REQUIRE(must_parse("1.5").as_double() == 1.5);
        CT_REQUIRE(must_parse("-0.0").is_floating());
        CT_REQUIRE(std::signbit(must_parse("-0.0").as_double()));
        CT_REQUIRE(must_parse("1e2").as_double() == 100.0);
        CT_REQUIRE(must_parse("1E-2").as_double() == 0.01);
        CT_REQUIRE(must_parse("0.1").as_double() == 0.1);
        CT_REQUIRE(must_parse("0.000123").as_double() == 0.000123);
        CT_REQUIRE(must_parse("123456.789").as_double() == 123456.789);
        CT_REQUIRE(must_parse("  \"hi\" \r\n\t").as_string() == "hi");
        CT_REQUIRE(must_parse("\"\"").as_string().empty());
        CT_REQUIRE(must_parse("7").as_double() == 7.0); // integers widen through as_double
    }

    void test_number_limits()
    {
        CT_REQUIRE(must_parse("9223372036854775807").as_int() == std::numeric_limits<std::int64_t>::max());
        CT_REQUIRE(must_parse("-9223372036854775808").as_int() == std::numeric_limits<std::int64_t>::min());
        CT_REQUIRE(must_parse("1234567890123456789").as_int() == 1234567890123456789LL); // 19 digits: fast path

        const value big = must_parse("9223372036854775808"); // one past int64 -> double
        CT_REQUIRE(big.is_floating());
        CT_REQUIRE(big.as_double() == 9223372036854775808.0);

        const value twenty = must_parse("12345678901234567890"); // 20 digits: from_chars path
        CT_REQUIRE(twenty.is_floating());
        CT_REQUIRE(twenty.as_double() == 12345678901234567890.0);

        const value huge = must_parse("123456789012345678901234567890");
        CT_REQUIRE(huge.is_floating());
        CT_REQUIRE(huge.as_double() == 1.2345678901234568e29);

        CT_REQUIRE(must_parse("1e22").as_double() == 1e22);                                       // Clinger edge
        CT_REQUIRE(must_parse("1e23").as_double() == 1e23);                                       // beyond Clinger
        CT_REQUIRE(must_parse("2.2250738585072014e-308").as_double() == 2.2250738585072014e-308); // min normal
        CT_REQUIRE(must_parse("1.7976931348623157e308").as_double() == 1.7976931348623157e308);   // max
        CT_REQUIRE(must_parse("4.9406564584124654e-324").as_double() == 4.9406564584124654e-324); // denormal
        CT_REQUIRE(must_parse("0.30000000000000004").as_double() == 0.30000000000000004);
        CT_REQUIRE(must_parse("1e-7").as_double() == 1e-7);

        CT_REQUIRE(must_fail("1e400") == parse_error_code::number_out_of_range);
        CT_REQUIRE(must_fail("-1e400") == parse_error_code::number_out_of_range);
    }

    void test_number_errors()
    {
        CT_REQUIRE(must_fail("-") == parse_error_code::unexpected_end);
        CT_REQUIRE(must_fail("- 1") == parse_error_code::invalid_number);
        CT_REQUIRE(must_fail("01") == parse_error_code::trailing_characters);
        CT_REQUIRE(must_fail("1.") == parse_error_code::unexpected_end);
        CT_REQUIRE(must_fail("1.x") == parse_error_code::invalid_number);
        CT_REQUIRE(must_fail("1e") == parse_error_code::unexpected_end);
        CT_REQUIRE(must_fail("1e+") == parse_error_code::unexpected_end);
        CT_REQUIRE(must_fail("1e+x") == parse_error_code::invalid_number);
        CT_REQUIRE(must_fail(".5") == parse_error_code::unexpected_character);
        CT_REQUIRE(must_fail("+1") == parse_error_code::unexpected_character);
        CT_REQUIRE(must_fail("0x10") == parse_error_code::trailing_characters);
        CT_REQUIRE(must_fail("Infinity") == parse_error_code::unexpected_character);
        CT_REQUIRE(must_fail("NaN") == parse_error_code::unexpected_character);
    }

    // -------------------------------------------------------------------------------------------
    // Strings
    // -------------------------------------------------------------------------------------------

    void test_strings()
    {
        CT_REQUIRE(must_parse(R"("a\"b\\c\/d\b\f\n\r\t")").as_string() == "a\"b\\c/d\b\f\n\r\t");
        CT_REQUIRE(must_parse(R"("\u0041\u00e9\u4e2d")").as_string() == "A\xC3\xA9\xE4\xB8\xAD");
        CT_REQUIRE(must_parse(R"("\ud83d\ude00")").as_string() == "\xF0\x9F\x98\x80"); // U+1F600
        CT_REQUIRE(must_parse(R"("\uD83D\uDE00")").as_string() == "\xF0\x9F\x98\x80"); // upper-case hex
        CT_REQUIRE(must_parse("\"h\xC3\xA9\"").as_string() == "h\xC3\xA9");           // raw UTF-8 passthrough
        CT_REQUIRE(must_parse(R"("\u0000")").as_string() == std::string(1, '\0'));

        // Long enough to exercise the SWAR loop on both sides of an escape.
        CT_REQUIRE(must_parse(R"("abcdefghijklmnop\nqrstuvwxyz0123456789")").as_string() ==
                   "abcdefghijklmnop\nqrstuvwxyz0123456789");
        const std::string long_body(1000, 'x');
        CT_REQUIRE(must_parse("\"" + long_body + "\"").as_string() == long_body);

        CT_REQUIRE(must_fail("\"abc") == parse_error_code::unterminated_string);
        CT_REQUIRE(must_fail("\"abc\\") == parse_error_code::unterminated_string);
        CT_REQUIRE(must_fail("\"\\x\"") == parse_error_code::invalid_escape);
        CT_REQUIRE(must_fail("\"\\u12\"") == parse_error_code::invalid_unicode_escape);
        CT_REQUIRE(must_fail("\"\\u12G4\"") == parse_error_code::invalid_unicode_escape);
        CT_REQUIRE(must_fail("\"\\ud83d\"") == parse_error_code::invalid_surrogate);
        CT_REQUIRE(must_fail("\"\\ud83d\\u0041\"") == parse_error_code::invalid_surrogate);
        CT_REQUIRE(must_fail("\"\\ude00\"") == parse_error_code::invalid_surrogate);
        CT_REQUIRE(must_fail("\"a\nb\"") == parse_error_code::control_character);
        CT_REQUIRE(must_fail("\"a\tb\"") == parse_error_code::control_character);
        CT_REQUIRE(must_fail("\"abcdefghijklmnop\x01\"") == parse_error_code::control_character);
    }

    // -------------------------------------------------------------------------------------------
    // Containers and structure errors
    // -------------------------------------------------------------------------------------------

    void test_containers()
    {
        const value v = must_parse(R"({"a": [1, 2.5, "x", null, true], "b": {"c": {}}, "d": []})");
        CT_REQUIRE(v.is_object());
        CT_REQUIRE(v.size() == 3);

        const value &a = v.at("a");
        CT_REQUIRE(a.is_array());
        CT_REQUIRE(a.size() == 5);
        CT_REQUIRE(a[0].as_int() == 1);
        CT_REQUIRE(a[1].as_double() == 2.5);
        CT_REQUIRE(a[2].as_string() == "x");
        CT_REQUIRE(a[3].is_null());
        CT_REQUIRE(a[4].as_bool());

        CT_REQUIRE(v["b"]["c"].is_object());
        CT_REQUIRE(v["b"]["c"].size() == 0);
        CT_REQUIRE(v.at("d").is_array());
        CT_REQUIRE(v.at("d").size() == 0);

        const value dup = must_parse(R"({"k":1,"k":2})");
        CT_REQUIRE(dup.size() == 2);
        CT_REQUIRE(dup.at("k").as_int() == 1); // first match wins

        CT_REQUIRE(must_parse("[]").is_array());
        CT_REQUIRE(must_parse("{}").is_object());
        CT_REQUIRE(must_parse(" [ ] ").size() == 0);
        CT_REQUIRE(must_parse("[[[[]]]]")[0][0][0].is_array());
    }

    void test_structure_errors()
    {
        CT_REQUIRE(must_fail("") == parse_error_code::unexpected_end);
        CT_REQUIRE(must_fail("   ") == parse_error_code::unexpected_end);
        CT_REQUIRE(must_fail("[1,]") == parse_error_code::unexpected_character);
        CT_REQUIRE(must_fail("[1 2]") == parse_error_code::expected_comma_or_bracket);
        CT_REQUIRE(must_fail("[1") == parse_error_code::unexpected_end);
        CT_REQUIRE(must_fail("[1,") == parse_error_code::unexpected_end);
        CT_REQUIRE(must_fail("]") == parse_error_code::unexpected_character);
        CT_REQUIRE(must_fail("{") == parse_error_code::unexpected_end);
        CT_REQUIRE(must_fail("{\"a\"") == parse_error_code::unexpected_end);
        CT_REQUIRE(must_fail("{\"a\":") == parse_error_code::unexpected_end);
        CT_REQUIRE(must_fail("{\"a\":1") == parse_error_code::unexpected_end);
        CT_REQUIRE(must_fail("{1:2}") == parse_error_code::expected_key);
        CT_REQUIRE(must_fail("{\"a\" 1}") == parse_error_code::expected_colon);
        CT_REQUIRE(must_fail("{\"a\":1 \"b\":2}") == parse_error_code::expected_comma_or_brace);
        CT_REQUIRE(must_fail("{\"a\":1,}") == parse_error_code::expected_key);
        CT_REQUIRE(must_fail("[] []") == parse_error_code::trailing_characters);
        CT_REQUIRE(must_fail("nul") == parse_error_code::invalid_literal);
        CT_REQUIRE(must_fail("tru") == parse_error_code::invalid_literal);
        CT_REQUIRE(must_fail("fals") == parse_error_code::invalid_literal);
        CT_REQUIRE(must_fail("nulll") == parse_error_code::trailing_characters);
        CT_REQUIRE(must_fail("True") == parse_error_code::unexpected_character);
    }

    void test_depth_limit()
    {
        std::string ok(max_depth, '[');
        ok.append(max_depth, ']');
        CT_REQUIRE(must_parse(ok).is_array());

        std::string too_deep(max_depth + 1, '[');
        too_deep.append(max_depth + 1, ']');
        CT_REQUIRE(must_fail(too_deep) == parse_error_code::nesting_too_deep);

        std::string objects;
        for (std::size_t i = 0; i < max_depth + 1; ++i)
            objects += "{\"a\":";
        objects += "1";
        objects.append(max_depth + 1, '}');
        CT_REQUIRE(must_fail(objects) == parse_error_code::nesting_too_deep);
    }

    void test_error_offsets()
    {
        auto r = parse("[1, 2, x]");
        CT_REQUIRE(!r);
        CT_REQUIRE(r.error().code == parse_error_code::unexpected_character);
        CT_REQUIRE(r.error().offset == 7);
        CT_REQUIRE(r.error().message() == "JSON parse error at offset 7: unexpected character");

        auto r2 = parse("{\"a\":1} x");
        CT_REQUIRE(!r2);
        CT_REQUIRE(r2.error() == (parse_error{parse_error_code::trailing_characters, 8}));

        auto r3 = parse("{\"a\":1,\n \"b\":\"unterminated");
        CT_REQUIRE(!r3);
        CT_REQUIRE(r3.error().code == parse_error_code::unterminated_string);
        CT_REQUIRE(r3.error().offset == 26);

        auto r4 = parse_document("[1, 2, x]");
        CT_REQUIRE(!r4);
        CT_REQUIRE(r4.error() == r.error()); // both DOMs share the parser, so the same error
    }

    // -------------------------------------------------------------------------------------------
    // Access API
    // -------------------------------------------------------------------------------------------

    void test_try_accessors()
    {
        value v = must_parse(R"({"n": 3, "f": 1.5, "s": "str", "b": true, "arr": [1], "nil": null})");

        CT_REQUIRE(v.find("n")->try_int() == 3);
        CT_REQUIRE(v.find("n")->try_double() == 3.0);
        CT_REQUIRE(v.find("n")->try_bool() == std::nullopt);
        CT_REQUIRE(v.find("n")->try_string() == std::nullopt);
        CT_REQUIRE(v.find("f")->try_int() == std::nullopt);
        CT_REQUIRE(v.find("f")->try_double() == 1.5);
        CT_REQUIRE(v.find("s")->try_string() == "str");
        CT_REQUIRE(v.find("s")->try_int() == std::nullopt);
        CT_REQUIRE(v.find("b")->try_bool() == true);
        CT_REQUIRE(v.find("nil")->try_bool() == std::nullopt);
        CT_REQUIRE(v.find("arr")->try_array() != nullptr);
        CT_REQUIRE(v.find("arr")->try_object() == nullptr);
        CT_REQUIRE(v.try_object() != nullptr);
        CT_REQUIRE(v.try_array() == nullptr);

        // Lookups are total: they never throw, whatever the receiver's type.
        CT_REQUIRE(v.find("missing") == nullptr);
        CT_REQUIRE(!v.contains("missing"));
        CT_REQUIRE(v.at("n").find("x") == nullptr);
        CT_REQUIRE(!v.at("arr").contains("x"));
        CT_REQUIRE(!value().contains("x"));

        // Mutable variants deduce a mutable result.
        if (value *p = v.find("n"))
            *p = 4;
        CT_REQUIRE(v.at("n").as_int() == 4);
        v.find("arr")->try_array()->push_back(value(2));
        CT_REQUIRE(v.at("arr").size() == 2);
        v.try_object()->emplace_back("added", value(true));
        CT_REQUIRE(v.at("added").as_bool());

        const value &cv = v;
        static_assert(std::is_same_v<decltype(cv.find("n")), const value *>);
        static_assert(std::is_same_v<decltype(v.find("n")), value *>);
        static_assert(std::is_same_v<decltype(cv.try_array()), const array *>);
        static_assert(std::is_same_v<decltype(v.try_array()), array *>);
    }

    void test_checked_access_throws()
    {
        value arr = must_parse("[1]");
        CT_REQUIRE(throws<type_error>([&] { (void)arr.as_object(); }));
        CT_REQUIRE(throws<type_error>([&] { (void)arr.as_string(); }));
        CT_REQUIRE(throws<type_error>([&] { (void)arr.as_int(); }));
        CT_REQUIRE(throws<type_error>([&] { (void)arr.at("x"); }));
        CT_REQUIRE(throws<type_error>([&] { (void)arr[0].as_bool(); }));
        CT_REQUIRE(throws<std::out_of_range>([&] { (void)arr[5]; }));

        const value obj = must_parse("{}");
        CT_REQUIRE(throws<std::out_of_range>([&] { (void)obj.at("x"); }));
        CT_REQUIRE(throws<std::out_of_range>([&] { (void)obj["x"]; }));
        CT_REQUIRE(throws<type_error>([&] { (void)obj[0]; }));

        // A mutable operator[] on a non-object, non-null value is a type error, not auto-vivification.
        CT_REQUIRE(throws<type_error>([&] { arr["k"] = 1; }));
    }

    void test_construction()
    {
        value v;
        CT_REQUIRE(v.is_null());
        v["a"] = 1; // null auto-vivifies to an object
        v["b"] = "two";
        v["c"] = {1, 2, 3};
        v["d"] = {{"x", 1.5}, {"y", nullptr}};
        v["a"] = 10; // existing key is replaced, not duplicated
        CT_REQUIRE(v.size() == 4);
        CT_REQUIRE(dump(v) == R"({"a":10,"b":"two","c":[1,2,3],"d":{"x":1.5,"y":null}})");

        const value arr = {1, "a", true};
        CT_REQUIRE(arr.is_array());
        CT_REQUIRE(arr.size() == 3);

        const value obj = {{"k", "v"}};
        CT_REQUIRE(obj.is_object());
        CT_REQUIRE(obj.at("k").as_string() == "v");

        const value pairs_but_not_object = {{"k", "v"}, {1, 2}}; // second pair has no string key
        CT_REQUIRE(pairs_but_not_object.is_array());

        CT_REQUIRE(value(std::string_view("sv")).as_string() == "sv");
        CT_REQUIRE(value(std::string("s")).as_string() == "s");
        CT_REQUIRE(value(3.0f).is_floating());
        CT_REQUIRE(value(std::uint8_t(7)).as_int() == 7);
        CT_REQUIRE(value(-7L).as_int() == -7);
        CT_REQUIRE(value(true).is_boolean());
        CT_REQUIRE(value(nullptr).is_null());
        CT_REQUIRE(value(array{}).is_array());
        CT_REQUIRE(value(object{}).is_object());
    }

    // -------------------------------------------------------------------------------------------
    // Serialization
    // -------------------------------------------------------------------------------------------

    void test_dump()
    {
        const std::string_view compact = R"({"a":[1,2.5,"x\n\"q\"",null,true,false],"b":{},"c":[]})";
        CT_REQUIRE(dump(must_parse(compact)) == compact);

        CT_REQUIRE(dump(value(1.0)) == "1.0");
        CT_REQUIRE(dump(value(-0.0)) == "-0.0");
        CT_REQUIRE(dump(value(0.1)) == "0.1");
        CT_REQUIRE(dump(value(1e21)) == "1e+21");
        CT_REQUIRE(dump(value(1e-7)) == "1e-07");
        CT_REQUIRE(dump(value(std::numeric_limits<double>::quiet_NaN())) == "null");
        CT_REQUIRE(dump(value(std::numeric_limits<double>::infinity())) == "null");
        CT_REQUIRE(dump(value(std::numeric_limits<std::int64_t>::min())) == "-9223372036854775808");

        CT_REQUIRE(dump(value(std::string("\x01\x1f"))) == R"("\u0001\u001f")");
        CT_REQUIRE(dump(value("\b\f\n\r\t\\\"/")) == R"("\b\f\n\r\t\\\"/")");
        CT_REQUIRE(dump(value("h\xC3\xA9")) == "\"h\xC3\xA9\""); // UTF-8 is emitted raw
        CT_REQUIRE(dump(value("abcdefghijklmnop\"qrstuvwxyz")) == R"("abcdefghijklmnop\"qrstuvwxyz")");

        CT_REQUIRE(dump(must_parse(R"({"a":[1,2],"b":{},"c":{"d":null}})"), 2) ==
                   "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": {},\n  \"c\": {\n    \"d\": null\n  }\n}");
        CT_REQUIRE(dump(must_parse("[]"), 4) == "[]");
        CT_REQUIRE(dump(must_parse("[1]"), 0) == "[\n1\n]");

        // Round trips are stable.
        for (std::string_view text : {compact, std::string_view(R"([0.1,1e+21,-5,"\u0001"])"),
                                      std::string_view(R"({"nested":{"deep":[[[]]]}})")})
        {
            const std::string once = dump(must_parse(text));
            CT_REQUIRE(once == text);
            CT_REQUIRE(dump(must_parse(once)) == once);
        }
    }

    // -------------------------------------------------------------------------------------------
    // Tape document and cursor
    // -------------------------------------------------------------------------------------------

    void test_document()
    {
        const std::string_view text = R"({"name":"x","vals":[1,2.5,true,null,"s"],"nested":{"k":[[]]}})";
        auto r = parse_document(text);
        CT_REQUIRE(r.has_value());
        const document &d = *r;
        CT_REQUIRE(!d.empty());

        const cursor root = d.root();
        CT_REQUIRE(root.valid());
        CT_REQUIRE(root.is_object());
        CT_REQUIRE(root.kind() == type::object);
        CT_REQUIRE(root.size() == 3);

        CT_REQUIRE(root.find("name").as_string() == "x");
        const cursor vals = root["vals"];
        CT_REQUIRE(vals.is_array());
        CT_REQUIRE(vals.size() == 5);
        CT_REQUIRE(vals[0].as_int() == 1);
        CT_REQUIRE(vals[1].as_double() == 2.5);
        CT_REQUIRE(vals[2].as_bool());
        CT_REQUIRE(vals[3].is_null());
        CT_REQUIRE(vals[4].as_string() == "s");
        CT_REQUIRE(vals[0].as_double() == 1.0);

        // Lookups are total on cursors too: an invalid cursor, never a throw.
        CT_REQUIRE(!root.find("missing").valid());
        CT_REQUIRE(root.contains("nested"));
        CT_REQUIRE(!vals.find("x").valid());
        CT_REQUIRE(!vals.contains("x"));

        // try_* mirror the value API.
        CT_REQUIRE(vals[0].try_int() == 1);
        CT_REQUIRE(vals[0].try_double() == 1.0);
        CT_REQUIRE(vals[0].try_string() == std::nullopt);
        CT_REQUIRE(vals[1].try_double() == 2.5);
        CT_REQUIRE(vals[1].try_int() == std::nullopt);
        CT_REQUIRE(vals[2].try_bool() == true);
        CT_REQUIRE(vals[3].try_bool() == std::nullopt);
        CT_REQUIRE(root["name"].try_string() == "x");

        // Iteration order and O(1) subtree skipping.
        std::size_t n = 0;
        for (const cursor el : vals.elements())
        {
            (void)el;
            ++n;
        }
        CT_REQUIRE(n == 5);
        std::string keys;
        for (const auto [key, val] : root.members())
        {
            keys += key;
            keys += ';';
            CT_REQUIRE(val.valid());
        }
        CT_REQUIRE(keys == "name;vals;nested;");
        CT_REQUIRE(root["nested"]["k"][0].is_array());
        CT_REQUIRE(root["nested"]["k"][0].size() == 0);
        CT_REQUIRE(root.first_child().as_string() == "name");
        CT_REQUIRE(root.first_child().next_sibling().as_string() == "x");

        // Checked access throws like the value API.
        CT_REQUIRE(throws<type_error>([&] { (void)vals.at("x"); }));
        CT_REQUIRE(throws<type_error>([&] { (void)vals[0].as_string(); }));
        CT_REQUIRE(throws<type_error>([&] { (void)root.elements(); }));
        CT_REQUIRE(throws<std::out_of_range>([&] { (void)root.at("missing"); }));
        CT_REQUIRE(throws<std::out_of_range>([&] { (void)vals[5]; }));

        // Serialization and conversion agree with the value tree.
        CT_REQUIRE(dump(d) == text);
        CT_REQUIRE(dump(vals) == R"([1,2.5,true,null,"s"])");
        CT_REQUIRE(dump(to_value(root)) == text);
        CT_REQUIRE(to_value(root["nested"]).is_object());
        CT_REQUIRE(to_value(vals[1]).as_double() == 2.5);
        CT_REQUIRE(dump(d, 1) == dump(must_parse(text), 1));

        const document empty;
        CT_REQUIRE(empty.empty());
        CT_REQUIRE(!empty.root().valid());
        CT_REQUIRE(dump(empty).empty());

        CT_REQUIRE(!parse_document("[1,").has_value());
        CT_REQUIRE(parse_document("\"\\ud83d\\ude00 and a long string that spans several words\"")->root().as_string() ==
                   "\xF0\x9F\x98\x80 and a long string that spans several words");
    }

    void test_document_matches_value()
    {
        for (std::string_view text : {std::string_view("null"), std::string_view("[]"), std::string_view("{}"),
                                      std::string_view(R"({"a":{"b":{"c":[1,[2,[3,[4]]]]}},"d":"\u00e9"})"),
                                      std::string_view(R"([-9223372036854775808,9223372036854775807,1e+300,-0.0])"),
                                      std::string_view(R"([{"":""},{"k":[{}]},[[[[[[[]]]]]]]])")})
        {
            const value v = must_parse(text);
            auto d = parse_document(text);
            CT_REQUIRE(d.has_value());
            CT_REQUIRE(dump(v) == dump(*d));
            // Escapes decode to raw UTF-8 on output, so only escape-free inputs round-trip byte for byte.
            const std::size_t esc = text.find("\\u");
            if (esc == std::string_view::npos)
                CT_REQUIRE(dump(v) == text);
            else
                CT_REQUIRE(dump(v) == std::string(text.substr(0, esc)) + "\xC3\xA9\"}");
        }
    }

    // -------------------------------------------------------------------------------------------
    // Scanner differential check: the SWAR fast path must agree with the scalar reference
    // -------------------------------------------------------------------------------------------

    void test_scan_differential()
    {
        using detail::scan::scalar;
        using detail::scan::swar;

        std::uint64_t state = 0x9E3779B97F4A7C15ULL;
        const auto next = [&]() -> std::uint32_t
        {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<std::uint32_t>(state >> 33);
        };

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
                    c = '"';
                    break;
                case 1:
                    c = '\\';
                    break;
                case 2:
                    c = static_cast<char>(next() % 0x20); // control byte
                    break;
                case 3:
                    c = static_cast<char>(0x80 + next() % 0x80); // UTF-8 byte: never a stop
                    break;
                case 4:
                    c = static_cast<char>(0x7F); // DEL is not a control byte for JSON
                    break;
                default:
                    c = static_cast<char>('a' + next() % 26);
                    break;
                }
                s.push_back(c);
            }
            for (std::size_t from = 0; from <= len; ++from)
                CT_REQUIRE(swar(s, from) == scalar(s, from));
        }

        // A single stop byte at every position of a long run.
        for (std::size_t pos = 0; pos < 41; ++pos)
        {
            for (char stop : {'"', '\\', '\n', '\0'})
            {
                std::string s(41, 'a');
                s[pos] = stop;
                CT_REQUIRE(swar(s, 0) == pos);
                CT_REQUIRE(scalar(s, 0) == pos);
            }
        }
        CT_REQUIRE(swar(std::string(64, 'z'), 0) == 64);
        CT_REQUIRE(swar("", 0) == 0);
    }

} // namespace

int main()
{
    test_scalars();
    test_number_limits();
    test_number_errors();
    test_strings();
    test_containers();
    test_structure_errors();
    test_depth_limit();
    test_error_offsets();
    test_try_accessors();
    test_checked_access_throws();
    test_construction();
    test_dump();
    test_document();
    test_document_matches_value();
    test_scan_differential();

    std::cout << "catalyst.resource.json: all tests passed\n";
    return 0;
}
