#include "../test_common.hpp"

#include <catalyst/resource/uri/uri.hpp>

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

using namespace catalyst::resource;

namespace
{
    uri must_parse(std::string_view text)
    {
        auto r = uri::parse(text);
        if (!r)
            std::cerr << "unexpected parse failure on <" << text << ">: " << r.error().message() << "\n";
        CT_REQUIRE(r.has_value());
        return std::move(*r);
    }

    uri_error_code must_fail(std::string_view text)
    {
        auto r = uri::parse(text);
        if (r)
            std::cerr << "unexpected parse success on <" << text << ">\n";
        CT_REQUIRE(!r.has_value());
        return r.error().code;
    }

    // Builds a parts aggregate without a partial designated-initializer list, which some compilers
    // warn about and which reads worse than naming the absent components anyway.
    uri::parts make_parts(std::optional<std::string_view> scheme, std::optional<std::string_view> authority,
                          std::string_view path, std::optional<std::string_view> query = std::nullopt,
                          std::optional<std::string_view> fragment = std::nullopt)
    {
        uri::parts p;
        p.scheme = scheme;
        p.authority = authority;
        p.path = path;
        p.query = query;
        p.fragment = fragment;
        return p;
    }

    // -------------------------------------------------------------------------------------------
    // Components
    // -------------------------------------------------------------------------------------------

    void test_full_uri()
    {
        const uri u = must_parse("foo://user@example.com:8042/over/there?name=ferret#nose");

        CT_REQUIRE(u.is_absolute());
        CT_REQUIRE(!u.is_relative());
        CT_REQUIRE(u.scheme() == "foo");
        CT_REQUIRE(u.authority() == "user@example.com:8042");
        CT_REQUIRE(u.userinfo() == "user");
        CT_REQUIRE(u.host() == "example.com");
        CT_REQUIRE(u.port() == "8042");
        CT_REQUIRE(u.port_number() == 8042);
        CT_REQUIRE(u.path() == "/over/there");
        CT_REQUIRE(u.query() == "name=ferret");
        CT_REQUIRE(u.fragment() == "nose");
        CT_REQUIRE(u.view() == "foo://user@example.com:8042/over/there?name=ferret#nose");
    }

    void test_absent_components()
    {
        const uri u = must_parse("pack:core/textures/stone.png");
        CT_REQUIRE(u.scheme() == "pack");
        CT_REQUIRE(!u.authority().has_value());
        CT_REQUIRE(!u.userinfo().has_value());
        CT_REQUIRE(!u.host().has_value());
        CT_REQUIRE(!u.port().has_value());
        CT_REQUIRE(u.path() == "core/textures/stone.png");
        CT_REQUIRE(!u.query().has_value());
        CT_REQUIRE(!u.fragment().has_value());
    }

    // An absent component and a present-but-empty one are different things, and the difference
    // survives a round trip through the text.
    void test_empty_is_not_absent()
    {
        const uri q = must_parse("s:p?");
        CT_REQUIRE(q.query().has_value());
        CT_REQUIRE(q.query()->empty());
        CT_REQUIRE(q.view() == "s:p?");

        const uri f = must_parse("s:p#");
        CT_REQUIRE(f.fragment().has_value());
        CT_REQUIRE(f.fragment()->empty());

        const uri none = must_parse("s:p");
        CT_REQUIRE(!none.query().has_value());
        CT_REQUIRE(!none.fragment().has_value());
    }

    void test_relative_references()
    {
        const uri path_only = must_parse("../textures/stone.png");
        CT_REQUIRE(path_only.is_relative());
        CT_REQUIRE(!path_only.scheme().has_value());
        CT_REQUIRE(path_only.path() == "../textures/stone.png");

        const uri net_path = must_parse("//example.com/p");
        CT_REQUIRE(net_path.is_relative());
        CT_REQUIRE(net_path.authority() == "example.com");
        CT_REQUIRE(net_path.path() == "/p");

        const uri query_only = must_parse("?a=1");
        CT_REQUIRE(query_only.path().empty());
        CT_REQUIRE(query_only.query() == "a=1");

        const uri fragment_only = must_parse("#top");
        CT_REQUIRE(fragment_only.path().empty());
        CT_REQUIRE(fragment_only.fragment() == "top");

        const uri empty;
        CT_REQUIRE(empty.empty());
        CT_REQUIRE(empty.is_relative());
        CT_REQUIRE(empty.path().empty());
        CT_REQUIRE(must_parse("").empty());
    }

    void test_authority_shapes()
    {
        // An empty authority is legal and is how file: URIs name the local machine.
        const uri local = must_parse("file:///c/data.csv");
        CT_REQUIRE(local.authority().has_value());
        CT_REQUIRE(local.authority()->empty());
        CT_REQUIRE(local.host()->empty());
        CT_REQUIRE(local.path() == "/c/data.csv");

        // An IPv6 literal keeps its brackets, or it would not be distinguishable from a name.
        const uri v6 = must_parse("http://[2001:db8::1]:8080/x");
        CT_REQUIRE(v6.host() == "[2001:db8::1]");
        CT_REQUIRE(v6.port_number() == 8080);

        // A host with no port, and a host with an empty port, differ.
        CT_REQUIRE(!must_parse("http://h/x").port().has_value());
        CT_REQUIRE(must_parse("http://h:/x").port()->empty());
        CT_REQUIRE(!must_parse("http://h:/x").port_number().has_value());

        // A literal '@' is not in the userinfo set, so a second one is not a userinfo that happens
        // to contain '@' -- it is a malformed authority, and saying so beats guessing which '@'
        // was meant to be the separator.
        CT_REQUIRE(must_fail("http://a@b@host/x") == uri_error_code::invalid_userinfo);

        // Encoded, it is content, and the split still finds the real separator.
        const uri at = must_parse("http://a%40b@host/x");
        CT_REQUIRE(at.userinfo() == "a%40b");
        CT_REQUIRE(at.host() == "host");
        CT_REQUIRE(*percent_decode(*at.userinfo()) == "a@b");
    }

    // -------------------------------------------------------------------------------------------
    // Validation
    // -------------------------------------------------------------------------------------------

    void test_parse_errors()
    {
        CT_REQUIRE(must_fail("1http://x") == uri_error_code::invalid_scheme);
        CT_REQUIRE(must_fail("ht tp:x") == uri_error_code::invalid_scheme);
        CT_REQUIRE(must_fail("h_t:x") == uri_error_code::invalid_scheme);

        CT_REQUIRE(must_fail("s:/a%zz") == uri_error_code::invalid_percent_encoding);
        CT_REQUIRE(must_fail("s:/a%2") == uri_error_code::invalid_percent_encoding);
        CT_REQUIRE(must_fail("s:/a%") == uri_error_code::invalid_percent_encoding);

        CT_REQUIRE(must_fail("s:/a b") == uri_error_code::invalid_path);
        CT_REQUIRE(must_fail("s:/a\x01") == uri_error_code::invalid_path);
        CT_REQUIRE(must_fail("s:/a[b") == uri_error_code::invalid_path);

        CT_REQUIRE(must_fail("s:?a b") == uri_error_code::invalid_query);
        CT_REQUIRE(must_fail("s:#a b") == uri_error_code::invalid_fragment);

        CT_REQUIRE(must_fail("http://h:80x/") == uri_error_code::invalid_port);
        CT_REQUIRE(must_fail("http://h:99999/") == uri_error_code::invalid_port);
        CT_REQUIRE(must_fail("http://h o/") == uri_error_code::invalid_host);
        CT_REQUIRE(must_fail("http://[2001:db8/") == uri_error_code::invalid_host);
        CT_REQUIRE(must_fail("http://[]/") == uri_error_code::invalid_host);
        CT_REQUIRE(must_fail("http://u ser@h/") == uri_error_code::invalid_userinfo);

        // path-noscheme: a leading ':' cannot be a scheme, so the path owns it -- and a path whose
        // first segment owns a ':' would reparse as a scheme, so it is refused.
        CT_REQUIRE(must_fail(":foo") == uri_error_code::relative_path_with_colon);
        CT_REQUIRE(must_fail(":") == uri_error_code::relative_path_with_colon);
        CT_REQUIRE(uri::parse("./a:b").has_value()); // the escape hatch RFC 3986 gives for it
    }

    void test_error_offsets()
    {
        auto r = uri::parse("s://h/ab%zz");
        CT_REQUIRE(!r.has_value());
        CT_REQUIRE(r.error().code == uri_error_code::invalid_percent_encoding);
        CT_REQUIRE(r.error().offset == 8); // the '%'

        auto p = uri::parse("http://h:12a/");
        CT_REQUIRE(!p.has_value());
        CT_REQUIRE(p.error().offset == 11); // the 'a'

        CT_REQUIRE(!(uri_error{uri_error_code::invalid_port, 3}.message().empty()));
    }

    // A colon in the first segment of a relative reference is rejected rather than silently
    // reparsed as a scheme, because silently reparsing is how a path turns into a protocol.
    void test_path_noscheme()
    {
        auto r = uri::parse("textures:stone/a.png");
        CT_REQUIRE(r.has_value());
        CT_REQUIRE(r->scheme() == "textures"); // a leading colon segment IS a scheme

        // With a slash first, there is no scheme to mistake it for, and the colon is content.
        CT_REQUIRE(uri::parse("a/b:c").has_value());
        CT_REQUIRE(uri::parse("/a:b").has_value());
    }

    // -------------------------------------------------------------------------------------------
    // Percent-encoding
    // -------------------------------------------------------------------------------------------

    void test_percent_coding()
    {
        CT_REQUIRE(percent_encode("abcXYZ019-._~") == "abcXYZ019-._~");
        CT_REQUIRE(percent_encode("a b") == "a%20b");
        CT_REQUIRE(percent_encode("a/b") == "a%2Fb");
        CT_REQUIRE(percent_encode("a/b", "/") == "a/b");
        CT_REQUIRE(percent_encode("a+b", uri::encode_set::path_segment) == "a+b");
        CT_REQUIRE(percent_encode("a+b", uri::encode_set::query_value) == "a%2Bb");
        CT_REQUIRE(percent_encode("a&b", uri::encode_set::query_value) == "a%26b");

        // Every byte above ASCII is escaped one byte at a time, so UTF-8 survives the round trip.
        const std::string utf8 = "\xE2\x9C\x93"; // U+2713
        CT_REQUIRE(percent_encode(utf8) == "%E2%9C%93");
        CT_REQUIRE(*percent_decode(percent_encode(utf8)) == utf8);

        CT_REQUIRE(*percent_decode("a%20b") == "a b");
        CT_REQUIRE(*percent_decode("a%2fb") == "a/b"); // lowercase hex decodes too
        CT_REQUIRE(*percent_decode("a+b") == "a+b");
        CT_REQUIRE(*percent_decode("a+b", true) == "a b");

        CT_REQUIRE(!percent_decode("a%zz").has_value());
        CT_REQUIRE(percent_decode("a%zz").error().code == uri_error_code::invalid_percent_encoding);
        CT_REQUIRE(percent_decode("a%zz").error().offset == 1);
        CT_REQUIRE(!percent_decode("%4").has_value());
    }

    void test_decoded_accessors()
    {
        const uri u = must_parse("s:/a%20b/c%2Fd?q=x%20y#f%20g");
        CT_REQUIRE(u.path() == "/a%20b/c%2Fd");
        CT_REQUIRE(*u.decoded_path() == "/a b/c/d");
        CT_REQUIRE(u.decoded_fragment() == "f g");
        CT_REQUIRE(!must_parse("s:p").decoded_fragment().has_value());

        const auto segs = u.path_segments();
        CT_REQUIRE(segs.size() == 2);
        CT_REQUIRE(segs[0] == "a%20b");
        CT_REQUIRE(segs[1] == "c%2Fd");
        CT_REQUIRE(must_parse("s:///").path_segments().empty());
    }

    // -------------------------------------------------------------------------------------------
    // Query parameters
    // -------------------------------------------------------------------------------------------

    void test_query_parameters()
    {
        const uri u = must_parse("s:p?a=1&flag&b=&c=3&a=2");
        const auto params = u.query_parameters();
        CT_REQUIRE(params.size() == 5);

        CT_REQUIRE(params[0].name == "a");
        CT_REQUIRE(params[0].value == "1");

        // No '=' at all is not the same as an empty value.
        CT_REQUIRE(params[1].name == "flag");
        CT_REQUIRE(!params[1].value.has_value());
        CT_REQUIRE(params[2].name == "b");
        CT_REQUIRE(params[2].value.has_value());
        CT_REQUIRE(params[2].value->empty());

        // Duplicates are kept, in order; find returns the first.
        CT_REQUIRE(params[4].name == "a");
        CT_REQUIRE(params[4].value == "2");
        CT_REQUIRE(u.find_query_parameter("a")->value == "1");
        CT_REQUIRE(!u.find_query_parameter("nope").has_value());

        CT_REQUIRE(must_parse("s:p").query_parameters().empty());
        CT_REQUIRE(must_parse("s:p?").query_parameters().empty());
        CT_REQUIRE(must_parse("s:p?a=1&&b=2").query_parameters().size() == 2);
        CT_REQUIRE(must_parse("s:p?a=1;b=2").query_parameters().size() == 2);
    }

    // -------------------------------------------------------------------------------------------
    // Composition
    // -------------------------------------------------------------------------------------------

    void test_from_parts()
    {
        auto full = uri::from_parts(
            make_parts("foo", "user@example.com:8042", "/over/there", "name=ferret", "nose"));
        CT_REQUIRE(full.has_value());
        CT_REQUIRE(full->view() == "foo://user@example.com:8042/over/there?name=ferret#nose");

        // An absent component leaves no trace; a present empty one still writes its delimiter.
        CT_REQUIRE(uri::from_parts(make_parts(std::nullopt, std::nullopt, ""))->view() == "");
        CT_REQUIRE(uri::from_parts(make_parts(std::nullopt, std::nullopt, "", ""))->view() == "?");
        CT_REQUIRE(uri::from_parts(make_parts("s", std::nullopt, "p"))->view() == "s:p");
        CT_REQUIRE(uri::from_parts(make_parts(std::nullopt, "", "/p"))->view() == "///p");

        // Composition is validated, so from_parts cannot mint a uri whose text would not reparse.
        CT_REQUIRE(!uri::from_parts(make_parts("1bad", std::nullopt, "p")).has_value());

        // Nor one whose text would reparse as something else. Each of these would compose text that
        // parses back with different components than were asked for.
        CT_REQUIRE(uri::from_parts(make_parts(std::nullopt, "h", "rootless")).error().code ==
                   uri_error_code::path_must_be_absolute); // would give authority "hrootless"
        CT_REQUIRE(uri::from_parts(make_parts(std::nullopt, std::nullopt, "//x")).error().code ==
                   uri_error_code::path_would_reparse); // would give authority "x"
        CT_REQUIRE(uri::from_parts(make_parts(std::nullopt, std::nullopt, "a:b")).error().code ==
                   uri_error_code::relative_path_with_colon); // would give scheme "a"
        CT_REQUIRE(uri::from_parts(make_parts("a:b", std::nullopt, "p")).error().code ==
                   uri_error_code::invalid_scheme); // would give scheme "a", path "b:"
        CT_REQUIRE(uri::from_parts(make_parts(std::nullopt, "h/x", "/p")).error().code ==
                   uri_error_code::invalid_host); // would give authority "h", path "/x/p"
    }

    // -------------------------------------------------------------------------------------------
    // Reference resolution: the RFC 3986 section 5.4 vectors
    // -------------------------------------------------------------------------------------------

    void check_resolve(const uri &base, std::string_view ref, std::string_view expected)
    {
        auto r = uri::resolve(base, must_parse(ref));
        if (!r)
            std::cerr << "resolve failed for <" << ref << ">: " << r.error().message() << "\n";
        CT_REQUIRE(r.has_value());
        if (r->view() != expected)
            std::cerr << "resolve <" << ref << "> gave <" << r->view() << ">, expected <" << expected << ">\n";
        CT_REQUIRE(r->view() == expected);
    }

    void test_resolve_normal()
    {
        const uri base = must_parse("http://a/b/c/d;p?q");

        check_resolve(base, "g:h", "g:h");
        check_resolve(base, "g", "http://a/b/c/g");
        check_resolve(base, "./g", "http://a/b/c/g");
        check_resolve(base, "g/", "http://a/b/c/g/");
        check_resolve(base, "/g", "http://a/g");
        check_resolve(base, "//g", "http://g");
        check_resolve(base, "?y", "http://a/b/c/d;p?y");
        check_resolve(base, "g?y", "http://a/b/c/g?y");
        check_resolve(base, "#s", "http://a/b/c/d;p?q#s");
        check_resolve(base, "g#s", "http://a/b/c/g#s");
        check_resolve(base, "g?y#s", "http://a/b/c/g?y#s");
        check_resolve(base, ";x", "http://a/b/c/;x");
        check_resolve(base, "g;x", "http://a/b/c/g;x");
        check_resolve(base, "g;x?y#s", "http://a/b/c/g;x?y#s");
        check_resolve(base, "", "http://a/b/c/d;p?q");
        check_resolve(base, ".", "http://a/b/c/");
        check_resolve(base, "./", "http://a/b/c/");
        check_resolve(base, "..", "http://a/b/");
        check_resolve(base, "../", "http://a/b/");
        check_resolve(base, "../g", "http://a/b/g");
        check_resolve(base, "../..", "http://a/");
        check_resolve(base, "../../", "http://a/");
        check_resolve(base, "../../g", "http://a/g");
    }

    void test_resolve_abnormal()
    {
        const uri base = must_parse("http://a/b/c/d;p?q");

        // Too many "..": the extra ones are discarded rather than escaping the authority.
        check_resolve(base, "../../../g", "http://a/g");
        check_resolve(base, "../../../../g", "http://a/g");
        check_resolve(base, "/./g", "http://a/g");
        check_resolve(base, "/../g", "http://a/g");

        // Dots that are part of a name, not a segment.
        check_resolve(base, "g.", "http://a/b/c/g.");
        check_resolve(base, ".g", "http://a/b/c/.g");
        check_resolve(base, "g..", "http://a/b/c/g..");
        check_resolve(base, "..g", "http://a/b/c/..g");

        check_resolve(base, "./../g", "http://a/b/g");
        check_resolve(base, "./g/.", "http://a/b/c/g/");
        check_resolve(base, "g/./h", "http://a/b/c/g/h");
        check_resolve(base, "g/../h", "http://a/b/c/h");
        check_resolve(base, "g;x=1/./y", "http://a/b/c/g;x=1/y");
        check_resolve(base, "g;x=1/../y", "http://a/b/c/y");

        // Dot segments in a query or fragment are not path syntax and are left alone.
        check_resolve(base, "g?y/./x", "http://a/b/c/g?y/./x");
        check_resolve(base, "g?y/../x", "http://a/b/c/g?y/../x");
        check_resolve(base, "g#s/./x", "http://a/b/c/g#s/./x");
        check_resolve(base, "g#s/../x", "http://a/b/c/g#s/../x");
    }

    // The case the resource system actually cares about: a manifest naming a sibling asset.
    void test_resolve_scheme_without_authority()
    {
        const uri manifest = must_parse("pack:core/materials/stone.json");
        auto tex = manifest.resolve(must_parse("../textures/stone_d.png"));
        CT_REQUIRE(tex.has_value());
        CT_REQUIRE(tex->view() == "pack:core/textures/stone_d.png");

        auto sibling = manifest.resolve(must_parse("granite.json"));
        CT_REQUIRE(sibling->view() == "pack:core/materials/granite.json");

        // A relative base has nothing to resolve against, and says so rather than guessing.
        auto bad = uri::resolve(must_parse("relative/base"), must_parse("x"));
        CT_REQUIRE(!bad.has_value());
        CT_REQUIRE(bad.error().code == uri_error_code::base_not_absolute);
    }

    // -------------------------------------------------------------------------------------------
    // Normalization and comparison
    // -------------------------------------------------------------------------------------------

    void test_normalized()
    {
        // Scheme and host lowercase; userinfo and path keep their case.
        CT_REQUIRE(must_parse("HTTP://User@Example.COM/Path").normalized().view() ==
                   "http://User@example.com/Path");

        // Escapes uppercase; escapes of unreserved bytes decode.
        CT_REQUIRE(must_parse("s:/%7ea/%2f").normalized().view() == "s:/~a/%2F");

        // Dot segments go.
        CT_REQUIRE(must_parse("s:/a/./b/../c").normalized().view() == "s:/a/c");

        // An empty path becomes "/" only when there is an authority to hang it from.
        CT_REQUIRE(must_parse("http://h").normalized().view() == "http://h/");
        CT_REQUIRE(must_parse("s:").normalized().view() == "s:");

        // An empty-but-present query or fragment keeps its delimiter: some schemes care.
        CT_REQUIRE(must_parse("http://h/p?").normalized().view() == "http://h/p?");
        CT_REQUIRE(must_parse("http://h/p#").normalized().view() == "http://h/p#");

        // Normalization is idempotent.
        const uri once = must_parse("HTTP://H/%7Ea/./b/..").normalized();
        CT_REQUIRE(once.normalized().view() == once.view());
    }

    void test_comparison_and_hash()
    {
        const uri a = must_parse("http://h/p");
        const uri b = must_parse("http://h/p");
        const uri c = must_parse("HTTP://H/p");

        CT_REQUIRE(a == b);
        CT_REQUIRE(!(a == c)); // operator== is byte-for-byte
        CT_REQUIRE(uri::equivalent(a, c));
        CT_REQUIRE(!uri::equivalent(a, must_parse("http://h/q")));

        CT_REQUIRE((a <=> b) == std::strong_ordering::equal);
        CT_REQUIRE(must_parse("a:1") < must_parse("b:1"));

        // Usable as a cache key.
        std::unordered_map<uri, int> cache;
        cache[a] = 1;
        CT_REQUIRE(cache.at(b) == 1);
        CT_REQUIRE(cache.find(c) == cache.end());
        cache[c.normalized()] = 2;
        CT_REQUIRE(cache.at(a) == 2);
    }

    void test_format()
    {
        const uri u = must_parse("s:p");
        CT_REQUIRE(std::format("{}", u) == "s:p");
        CT_REQUIRE(std::format("[{:>5}]", u) == "[  s:p]");
    }

    // Anything that parses must reproduce its own text exactly; that is what makes the offsets
    // trustworthy and what lets a uri be stored and re-read.
    void test_round_trip()
    {
        static constexpr std::string_view samples[] = {
            "",
            "s:",
            "s:p",
            "s:/p",
            "s://",
            "s:///p",
            "//h",
            "//h/p",
            "/p",
            "p",
            "./p",
            "?q",
            "#f",
            "s://u@h:1/p?q#f",
            "s://h/p?#",
            "pack:core/textures/stone.png",
            "http://[::1]:8080/a/b?c=d#e",
            "s:/%41%2F%7e",
        };

        for (const std::string_view s : samples)
        {
            const uri u = must_parse(s);
            CT_REQUIRE(u.view() == s);

            // Reassembling from the pieces it reports must give the same text back.
            uri::parts p;
            p.scheme = u.scheme();
            p.authority = u.authority();
            p.path = u.path();
            p.query = u.query();
            p.fragment = u.fragment();
            auto rebuilt = uri::from_parts(p);
            CT_REQUIRE(rebuilt.has_value());
            if (rebuilt->view() != s)
                std::cerr << "round trip of <" << s << "> gave <" << rebuilt->view() << ">\n";
            CT_REQUIRE(rebuilt->view() == s);
        }
    }

} // namespace

int main()
{
    test_full_uri();
    test_absent_components();
    test_empty_is_not_absent();
    test_relative_references();
    test_authority_shapes();
    test_parse_errors();
    test_error_offsets();
    test_path_noscheme();
    test_percent_coding();
    test_decoded_accessors();
    test_query_parameters();
    test_from_parts();
    test_resolve_normal();
    test_resolve_abnormal();
    test_resolve_scheme_without_authority();
    test_normalized();
    test_comparison_and_hash();
    test_format();
    test_round_trip();

    std::cout << "catalyst.resource.uri: all tests passed\n";
    return 0;
}
