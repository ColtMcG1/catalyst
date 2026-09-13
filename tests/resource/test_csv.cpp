#include "../test_common.hpp"

#include <catalyst/resource/csv/csv.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace catalyst::resource::csv;

namespace
{
    table must_parse(std::string_view text, const dialect &d = dialect{})
    {
        auto r = parse_table(text, d);
        if (!r)
            std::cerr << "unexpected parse failure on <" << text << ">: " << r.error().message() << "\n";
        CT_REQUIRE(r.has_value());
        return std::move(*r);
    }

    parse_error_code must_fail(std::string_view text, const dialect &d = dialect{})
    {
        auto r = parse_table(text, d);
        if (r)
            std::cerr << "unexpected parse success on <" << text << ">\n";
        CT_REQUIRE(!r.has_value());
        return r.error().code;
    }

    dialect headerless()
    {
        dialect d;
        d.has_header = false;
        return d;
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
    // Shape
    // -------------------------------------------------------------------------------------------

    void test_basic()
    {
        const table t = must_parse("name,hp,speed\ngoblin,12,3.5\ntroll,40,1.25\n");

        CT_REQUIRE(t.has_header());
        CT_REQUIRE(t.columns() == 3);
        CT_REQUIRE(t.size() == 2); // the header is not a data row
        CT_REQUIRE(!t.empty());
        CT_REQUIRE(t.field_count() == 6);

        CT_REQUIRE(t.header().size() == 3);
        CT_REQUIRE(t.header()[0] == "name");
        CT_REQUIRE(t.header()[2] == "speed");

        CT_REQUIRE(t.row(0)[0] == "goblin");
        CT_REQUIRE(t.row(0)[1] == "12");
        CT_REQUIRE(t.row(1)[0] == "troll");

        // ... and by column name.
        CT_REQUIRE(t.row(0)["name"] == "goblin");
        CT_REQUIRE(t.row(1)["speed"] == "1.25");
        CT_REQUIRE(t.column_index("hp") == 1);
        CT_REQUIRE(!t.column_index("mana").has_value());
        CT_REQUIRE(t.contains_column("hp"));
        CT_REQUIRE(!t.contains_column("mana"));
        CT_REQUIRE(t.row(0).contains("hp"));
    }

    void test_headerless()
    {
        const table t = must_parse("goblin,12\ntroll,40\n", headerless());
        CT_REQUIRE(!t.has_header());
        CT_REQUIRE(t.header().empty());
        CT_REQUIRE(t.size() == 2);
        CT_REQUIRE(t.columns() == 2);
        CT_REQUIRE(t.row(0)[0] == "goblin");

        // With no header there is nothing to look a name up in, and asking says so.
        CT_REQUIRE(!t.column_index("goblin").has_value());
        CT_REQUIRE(t.row(0)["goblin"].empty());
        CT_REQUIRE(throws<std::out_of_range>([&] { (void)t.row(0).at("goblin"); }));
    }

    void test_edges()
    {
        // Empty input is an empty table, not an error.
        const table none = must_parse("");
        CT_REQUIRE(none.empty());
        CT_REQUIRE(none.size() == 0);
        CT_REQUIRE(none.columns() == 0);
        CT_REQUIRE(!none.has_header());
        CT_REQUIRE(none.row(0).empty()); // out of range is an empty row, not a crash

        // A file with only a header has columns but no rows.
        const table header_only = must_parse("a,b,c\n");
        CT_REQUIRE(header_only.empty());
        CT_REQUIRE(header_only.has_header());
        CT_REQUIRE(header_only.columns() == 3);

        // The last record need not be terminated.
        CT_REQUIRE(must_parse("a,b\n1,2").size() == 1);
        CT_REQUIRE(must_parse("a,b\n1,2\n").size() == 1);

        // Empty fields are fields.
        const table empties = must_parse("a,b,c\n,,\n");
        CT_REQUIRE(empties.row(0).size() == 3);
        CT_REQUIRE(empties.row(0)[0].empty());
        CT_REQUIRE(empties.row(0)[2].empty());

        // One column is a table too.
        const table single = must_parse("a\n1\n2\n");
        CT_REQUIRE(single.size() == 2);
        CT_REQUIRE(single.columns() == 1);
        CT_REQUIRE(single.row(1)[0] == "2");
    }

    void test_line_endings()
    {
        // CRLF, LF and a lone CR all end a record; a file that has been through a text editor may
        // contain any of them.
        for (const std::string_view nl : {"\r\n", "\n", "\r"})
        {
            std::string text = "a,b";
            text += nl;
            text += "1,2";
            text += nl;
            text += "3,4";
            text += nl;

            const table t = must_parse(text);
            CT_REQUIRE(t.size() == 2);
            CT_REQUIRE(t.row(0)[1] == "2");
            CT_REQUIRE(t.row(1)[0] == "3");
        }

        // Mixed within one file, because that happens too.
        const table mixed = must_parse("a,b\r\n1,2\n3,4\r5,6");
        CT_REQUIRE(mixed.size() == 3);
        CT_REQUIRE(mixed.row(2)[1] == "6");
    }

    void test_blank_lines()
    {
        const table t = must_parse("a,b\n\n1,2\n\n\n3,4\n\n");
        CT_REQUIRE(t.size() == 2);
        CT_REQUIRE(t.row(0)[0] == "1");
        CT_REQUIRE(t.row(1)[0] == "3");

        // A quoted empty field is a real one-field record, not a blank line.
        dialect one_column = headerless();
        const table quoted = must_parse("\"\"\n", one_column);
        CT_REQUIRE(quoted.size() == 1);
        CT_REQUIRE(quoted.row(0).size() == 1);
        CT_REQUIRE(quoted.row(0)[0].empty());

        // Keeping blank lines makes them one-field records.
        dialect keep = headerless();
        keep.skip_blank_lines = false;
        keep.allow_ragged = true;
        const table kept = must_parse("1,2\n\n3,4\n", keep);
        CT_REQUIRE(kept.size() == 3);
        CT_REQUIRE(kept.row(1).size() == 1);
        CT_REQUIRE(kept.row(1)[0].empty());
    }

    // -------------------------------------------------------------------------------------------
    // Quoting
    // -------------------------------------------------------------------------------------------

    void test_quoting()
    {
        // A quoted field may hold the delimiter, a newline, and quotes written twice.
        const table t = must_parse("a,b\n\"x,y\",\"line1\nline2\"\n\"he said \"\"hi\"\"\",plain\n");
        CT_REQUIRE(t.size() == 2);
        CT_REQUIRE(t.row(0)[0] == "x,y");
        CT_REQUIRE(t.row(0)[1] == "line1\nline2");
        CT_REQUIRE(t.row(1)[0] == "he said \"hi\"");
        CT_REQUIRE(t.row(1)[1] == "plain");

        // A field that is nothing but doubled quotes.
        const table q = must_parse("a\n\"\"\"\"\n");
        CT_REQUIRE(q.row(0)[0] == "\"");

        // A quote is only special at the start of a field, and CRLF inside quotes stays as written.
        const table crlf = must_parse("a\n\"x\r\ny\"\n");
        CT_REQUIRE(crlf.row(0)[0] == "x\r\ny");
    }

    void test_quoting_errors()
    {
        CT_REQUIRE(must_fail("a,b\n\"unterminated,2\n") == parse_error_code::unterminated_quote);
        CT_REQUIRE(must_fail("a,b\n1,va\"lue\n") == parse_error_code::bare_quote);
        CT_REQUIRE(must_fail("a,b\n\"ab\"c,2\n") == parse_error_code::invalid_quoted_escape);

        // The error says which record and which field, which is what someone with the file open needs.
        auto r = parse_table("a,b\n1,2\n3,va\"lue\n");
        CT_REQUIRE(!r.has_value());
        CT_REQUIRE(r.error().code == parse_error_code::bare_quote);
        CT_REQUIRE(r.error().line == 3);
        CT_REQUIRE(r.error().column == 2);
        CT_REQUIRE(r.error().offset == 12); // the quote itself
        CT_REQUIRE(!r.error().message().empty());
    }

    void test_ragged()
    {
        CT_REQUIRE(must_fail("a,b,c\n1,2\n") == parse_error_code::inconsistent_column_count);
        CT_REQUIRE(must_fail("a,b\n1,2,3\n") == parse_error_code::inconsistent_column_count);

        // The header fixes the width, so a short row is caught against it.
        auto r = parse_table("a,b,c\n1,2,3\n4,5\n");
        CT_REQUIRE(!r.has_value());
        CT_REQUIRE(r.error().line == 3);

        dialect ragged;
        ragged.allow_ragged = true;
        const table t = must_parse("a,b,c\n1,2,3\n4,5\n6,7,8,9\n", ragged);
        CT_REQUIRE(t.size() == 3);
        CT_REQUIRE(t.row(0).size() == 3);
        CT_REQUIRE(t.row(1).size() == 2);
        CT_REQUIRE(t.row(2).size() == 4);

        // A row that stops early has no value for the columns it stopped before; reading one is an
        // empty field rather than a fault, because on a ragged table that is routine.
        CT_REQUIRE(t.row(1)[2].empty());
        CT_REQUIRE(t.row(1)["c"].empty());
        CT_REQUIRE(throws<std::out_of_range>([&] { (void)t.row(1).at(2); }));
    }

    // -------------------------------------------------------------------------------------------
    // Dialects
    // -------------------------------------------------------------------------------------------

    void test_dialects()
    {
        const table tsv = must_parse("a\tb\n1\t2\n", dialect::tab());
        CT_REQUIRE(tsv.columns() == 2);
        CT_REQUIRE(tsv.row(0)[1] == "2");

        const table semi = must_parse("a;b\n1;2\n", dialect::semicolon());
        CT_REQUIRE(semi.row(0)[0] == "1");

        // A delimiter with no SWAR specialization takes the scalar path and must agree.
        dialect hashed;
        hashed.delimiter = '#';
        const table hash = must_parse("a#b\n1#2\n", hashed);
        CT_REQUIRE(hash.columns() == 2);
        CT_REQUIRE(hash.row(0)[1] == "2");

        // So must a non-standard quote.
        dialect tick;
        tick.quote = '\'';
        const table t = must_parse("a,b\n'x,y',2\n", tick);
        CT_REQUIRE(t.row(0)[0] == "x,y");
        CT_REQUIRE(t.row(0)[1] == "2");

        // With a non-standard quote, a double quote is ordinary content.
        const table dq = must_parse("a\nsay \"hi\"\n", tick);
        CT_REQUIRE(dq.row(0)[0] == "say \"hi\"");
    }

    void test_trimming()
    {
        dialect trim;
        trim.trim_whitespace = true;

        const table t = must_parse("a , b \n  1  ,\t2\t\n", trim);
        CT_REQUIRE(t.header()[0] == "a");
        CT_REQUIRE(t.header()[1] == "b");
        CT_REQUIRE(t.row(0)[0] == "1");
        CT_REQUIRE(t.row(0)[1] == "2");

        // Quoting is how a file says the whitespace is data, so a quoted field is never trimmed.
        const table q = must_parse("a,b\n\"  x  \" , y\n", trim);
        CT_REQUIRE(q.row(0)[0] == "  x  ");
        CT_REQUIRE(q.row(0)[1] == "y");

        // A field of nothing but whitespace trims to empty.
        const table blank = must_parse("a,b\n   ,x\n", trim);
        CT_REQUIRE(blank.row(0)[0].empty());
        CT_REQUIRE(blank.row(0)[1] == "x");

        // Without trimming, the spaces are content.
        CT_REQUIRE(must_parse("a,b\n  1  ,2\n").row(0)[0] == "  1  ");
    }

    // -------------------------------------------------------------------------------------------
    // Typed access
    // -------------------------------------------------------------------------------------------

    void test_typed_fields()
    {
        const table t = must_parse("i,d,b,junk\n-42,3.5,TRUE,hello\n", headerless() == dialect{} ? dialect{}
                                                                                                 : dialect{});

        CT_REQUIRE(t.row(0)["i"].try_int() == -42);
        CT_REQUIRE(t.row(0)["i"].as_int() == -42);
        CT_REQUIRE(t.row(0)["d"].try_double() == 3.5);
        CT_REQUIRE(t.row(0)["b"].try_bool() == true);

        // A field is text until proven otherwise, and every conversion can decline.
        CT_REQUIRE(!t.row(0)["junk"].try_int().has_value());
        CT_REQUIRE(!t.row(0)["junk"].try_double().has_value());
        CT_REQUIRE(!t.row(0)["junk"].try_bool().has_value());

        // Partial matches are not matches: the whole field must convert.
        CT_REQUIRE(!field("12abc").try_int().has_value());
        CT_REQUIRE(!field("3.5x").try_double().has_value());
        CT_REQUIRE(!field("").try_int().has_value());
        CT_REQUIRE(!field(" 12").try_int().has_value());

        CT_REQUIRE(field("0").try_int() == 0);
        CT_REQUIRE(field("9223372036854775807").try_int() == INT64_MAX);
        CT_REQUIRE(!field("9223372036854775808").try_int().has_value()); // overflows int64
        CT_REQUIRE(field("-1e3").try_double() == -1000.0);

        for (const std::string_view yes : {"true", "TRUE", "True", "yes", "Y", "1", "t"})
            CT_REQUIRE(field(yes).try_bool() == true);
        for (const std::string_view no : {"false", "FALSE", "no", "N", "0", "f"})
            CT_REQUIRE(field(no).try_bool() == false);
        CT_REQUIRE(!field("maybe").try_bool().has_value());

        // The checked accessors throw where the try_ ones decline.
        CT_REQUIRE(throws<type_error>([] { (void)field("x").as_int(); }));
        CT_REQUIRE(throws<type_error>([] { (void)field("x").as_double(); }));
        CT_REQUIRE(throws<type_error>([] { (void)field("x").as_bool(); }));
    }

    void test_iteration()
    {
        const table t = must_parse("a,b\n1,2\n3,4\n5,6\n");

        std::string seen;
        for (const row r : t)
            for (const field f : r)
                seen.append(f.view());
        CT_REQUIRE(seen == "123456");

        CT_REQUIRE(t.end() - t.begin() == 3);
        CT_REQUIRE((*(t.begin() + 2))[0] == "5");
        CT_REQUIRE(t.at(1)[1] == "4");
        CT_REQUIRE(throws<std::out_of_range>([&] { (void)t.at(3); }));

        const row r = t.row(0);
        CT_REQUIRE(r.end() - r.begin() == 2);
        CT_REQUIRE(r.at(0) == "1");
    }

    // -------------------------------------------------------------------------------------------
    // Writing
    // -------------------------------------------------------------------------------------------

    void test_needs_quoting()
    {
        const dialect d;
        CT_REQUIRE(!needs_quoting("plain", d));
        CT_REQUIRE(!needs_quoting("", d));
        CT_REQUIRE(!needs_quoting("  spaced  ", d)); // significant only when the dialect trims
        CT_REQUIRE(needs_quoting("a,b", d));
        CT_REQUIRE(needs_quoting("a\"b", d));
        CT_REQUIRE(needs_quoting("a\nb", d));
        CT_REQUIRE(needs_quoting("a\rb", d));

        dialect trim;
        trim.trim_whitespace = true;
        CT_REQUIRE(needs_quoting("  spaced  ", trim));
        CT_REQUIRE(needs_quoting("x\t", trim));
        CT_REQUIRE(!needs_quoting("plain", trim));

        // Under a tab dialect it is the tab, not the comma, that forces quoting.
        CT_REQUIRE(!needs_quoting("a,b", dialect::tab()));
        CT_REQUIRE(needs_quoting("a\tb", dialect::tab()));
    }

    void test_writer()
    {
        dialect lf;
        lf.newline = line_ending::lf;

        writer w(lf);
        w.field("name").field("note").end_row();
        w.field("goblin").field("says \"hi\"").end_row();
        w.field("a,b").field("x\ny").end_row();

        CT_REQUIRE(w.str() == "name,note\ngoblin,\"says \"\"hi\"\"\"\n\"a,b\",\"x\ny\"\n");

        const std::string taken = w.take();
        CT_REQUIRE(!taken.empty());
        CT_REQUIRE(w.empty());

        // CRLF is the default, per RFC 4180.
        writer crlf;
        crlf.field("a").end_row();
        CT_REQUIRE(crlf.str() == "a\r\n");
    }

    void test_dump_round_trip()
    {
        static constexpr std::string_view samples[] = {
            "a,b,c\r\n1,2,3\r\n",
            "a,b\r\n\"x,y\",\"line1\nline2\"\r\n",
            "a\r\n\"he said \"\"hi\"\"\"\r\n",
            "a,b\r\n,\r\n",
            "only,a,header\r\n",
        };

        for (const std::string_view s : samples)
        {
            const table t = must_parse(s);
            const std::string written = dump(t);
            if (written != s)
                std::cerr << "round trip of <" << s << "> gave <" << written << ">\n";
            CT_REQUIRE(written == s);

            // And the text it wrote must parse back to the same table.
            const table again = must_parse(written);
            CT_REQUIRE(again.size() == t.size());
            CT_REQUIRE(again.header() == t.header());
            for (std::size_t r = 0; r < t.size(); ++r)
                for (std::size_t c = 0; c < t.row(r).size(); ++c)
                    CT_REQUIRE(again.row(r)[c] == t.row(r)[c]);
        }

        // Reading with one dialect and writing with another converts between flavours.
        const table t = must_parse("a,b\r\n1,2\r\n");
        dialect tsv = dialect::tab();
        tsv.newline = line_ending::lf;
        CT_REQUIRE(dump(t, tsv) == "a\tb\n1\t2\n");

        // A headerless table writes no header.
        const table h = must_parse("1,2\r\n", headerless());
        CT_REQUIRE(dump(h) == "1,2\r\n");
    }

    // -------------------------------------------------------------------------------------------
    // The SWAR fast path must agree with the scalar fallback
    // -------------------------------------------------------------------------------------------

    // '|' has a SWAR specialization and '#' does not, so parsing the same content under each and
    // comparing pins the two scanners against one another -- the CSV counterpart of the JSON
    // module's differential scan test. Fields are deliberately longer than the 8-byte SWAR stride
    // and of varying length, so both the word loop and its unaligned tail are exercised.
    void test_scan_differential()
    {
        std::string swar_text;
        std::string scalar_text;
        std::vector<std::string> expected;

        for (int r = 0; r < 40; ++r)
        {
            for (int c = 0; c < 4; ++c)
            {
                std::string cell(static_cast<std::size_t>((r * 4 + c) % 23), 'x');
                cell += std::to_string(r * 4 + c);
                expected.push_back(cell);

                if (c != 0)
                {
                    swar_text += '|';
                    scalar_text += '#';
                }
                swar_text += cell;
                scalar_text += cell;
            }
            swar_text += '\n';
            scalar_text += '\n';
        }

        dialect pipe = headerless();
        pipe.delimiter = '|';
        dialect hash = headerless();
        hash.delimiter = '#';

        const table a = must_parse(swar_text, pipe);
        const table b = must_parse(scalar_text, hash);

        CT_REQUIRE(a.size() == 40);
        CT_REQUIRE(a.size() == b.size());

        std::size_t i = 0;
        for (std::size_t r = 0; r < a.size(); ++r)
        {
            CT_REQUIRE(a.row(r).size() == 4);
            CT_REQUIRE(b.row(r).size() == 4);
            for (std::size_t c = 0; c < 4; ++c, ++i)
            {
                CT_REQUIRE(a.row(r)[c] == b.row(r)[c]);
                CT_REQUIRE(a.row(r)[c] == expected[i]);
            }
        }
    }

    // A table big enough to have forced the buffers to grow, to catch any offset that was captured
    // before a reallocation.
    void test_large_table()
    {
        std::string text = "id,name\n";
        for (int i = 0; i < 5000; ++i)
        {
            text += std::to_string(i);
            text += ",\"name ";
            text += std::to_string(i);
            text += ", with a comma\"\n";
        }

        const table t = must_parse(text);
        CT_REQUIRE(t.size() == 5000);
        CT_REQUIRE(t.row(0)["name"] == "name 0, with a comma");
        CT_REQUIRE(t.row(4999)["id"] == "4999");
        CT_REQUIRE(t.row(4999)["name"] == "name 4999, with a comma");
        CT_REQUIRE(t.row(2500)["id"].try_int() == 2500);
    }

} // namespace

int main()
{
    test_basic();
    test_headerless();
    test_edges();
    test_line_endings();
    test_blank_lines();
    test_quoting();
    test_quoting_errors();
    test_ragged();
    test_dialects();
    test_trimming();
    test_typed_fields();
    test_iteration();
    test_needs_quoting();
    test_writer();
    test_dump_round_trip();
    test_scan_differential();
    test_large_table();

    std::cout << "catalyst.resource.csv: all tests passed\n";
    return 0;
}
