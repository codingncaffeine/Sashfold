#include "Test.h"

#include "core/Json.h"

using namespace sashfold;

int main()
{
    // Basics.
    auto doc = JsonValue::parse(R"({"a": [1, 2.5, -3], "b": "x\ny", "c": true, "d": null})");
    CHECK(doc.has_value());
    if (doc) {
        CHECK(doc->is_object());
        JsonValue const* a = doc->get("a");
        CHECK(a && a->is_array());
        if (a && a->is_array()) {
            CHECK_EQ(a->as_array().size(), std::size_t { 3 });
            CHECK_EQ(a->as_array()[1].as_number(), 2.5);
            CHECK_EQ(a->as_array()[2].as_number(), -3.0);
        }
        JsonValue const* b = doc->get("b");
        CHECK(b && b->is_string());
        if (b)
            CHECK_EQ(b->as_string(), std::string("x\ny"));
        CHECK(doc->get("c") && doc->get("c")->as_bool());
        CHECK(doc->get("d") && doc->get("d")->is_null());
        CHECK(doc->get("missing") == nullptr);
    }

    // \u escapes: BMP, surrogate pair, lone surrogate survives (WTF-8).
    auto text = JsonValue::parse(R"(["Aé", "😀", "\ud800"])");
    CHECK(text.has_value());
    if (text) {
        auto const& array = text->as_array();
        CHECK_EQ(array[0].as_string(), std::string("A\xC3\xA9"));
        CHECK_EQ(array[1].as_string(), std::string("\xF0\x9F\x98\x80"));
        CHECK_EQ(array[2].as_string().size(), std::size_t { 3 }); // lone surrogate, WTF-8 encoded
    }

    // Malformed documents are rejected, not half-parsed.
    CHECK(!JsonValue::parse("{").has_value());
    CHECK(!JsonValue::parse("[1,]").has_value());
    CHECK(!JsonValue::parse("[1] garbage").has_value());
    CHECK(!JsonValue::parse(R"({"a" 1})").has_value());
    CHECK(!JsonValue::parse("nul").has_value());

    // Nested structures.
    auto nested = JsonValue::parse(R"({"tests": [{"input": "<a>", "output": [["StartTag", "a", {}]]}]})");
    CHECK(nested.has_value());
    if (nested) {
        JsonValue const* tests = nested->get("tests");
        CHECK(tests && tests->is_array() && tests->as_array().size() == 1);
        if (tests && !tests->as_array().empty()) {
            JsonValue const* input = tests->as_array()[0].get("input");
            CHECK(input && input->as_string() == "<a>");
        }
    }

    return sashfold::test::report("json");
}
