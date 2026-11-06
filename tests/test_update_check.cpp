#include "Test.h"

#include "ui/UpdateCheck.h"

using namespace sashfold;
using namespace sashfold::ui;

int main()
{
    // --- versions --------------------------------------------------------------
    CHECK(parse_release_version("0.2.1").has_value());
    CHECK(parse_release_version("v10.20.30").has_value());
    CHECK_EQ((*parse_release_version("v10.20.30"))[1], std::uint32_t { 20 });
    for (char const* const bad : { "", "v", "0.2", "0.2.1.4", "0.2.x", "0..1", ".2.1", "0.2.", "0.3.0-rc1", "0.3 (preview)", "1.2.3 ", "-1.2.3", "1234567890.1.1" })
        CHECK(!parse_release_version(bad).has_value());

    CHECK(is_newer_release("0.2.1", "0.2.0"));
    CHECK(is_newer_release("v0.3.0", "0.2.9"));
    CHECK(is_newer_release("1.0.0", "0.99.99"));
    CHECK(is_newer_release("0.10.0", "0.9.0")); // numbers, not text
    CHECK(!is_newer_release("0.2.0", "0.2.0"));
    CHECK(!is_newer_release("0.1.9", "0.2.0"));
    CHECK(!is_newer_release("0.3.0-rc1", "0.2.0"));
    CHECK(!is_newer_release("0.3.0", "a build"));

    // --- the answer -------------------------------------------------------------
    CHECK_EQ(release_named_by("https://github.com/codingncaffeine/Sashfold/releases/tag/v0.2.1").value_or("none"), std::string("0.2.1"));
    CHECK_EQ(release_named_by("/codingncaffeine/Sashfold/releases/tag/v1.0.0?from=latest#top").value_or("none"), std::string("1.0.0"));
    CHECK_EQ(release_named_by("https://github.com/codingncaffeine/Sashfold/releases/tag/v01.2.03").value_or("none"), std::string("1.2.3"));
    CHECK(!release_named_by("https://github.com/codingncaffeine/Sashfold/releases").has_value());
    CHECK(!release_named_by("https://github.com/codingncaffeine/Sashfold/releases/tag/nightly").has_value());
    CHECK(!release_named_by("https://github.com/login?return_to=x").has_value());
    CHECK(!release_named_by("").has_value());
    CHECK_EQ(release_page_address("0.2.1"), std::string("https://github.com/codingncaffeine/Sashfold/releases/tag/v0.2.1"));

    // --- what the profile keeps ------------------------------------------------
    {
        UpdateState const none = parse_update_state("");
        CHECK_EQ(none.checked, std::int64_t { 0 });
        CHECK(none.latest.empty());
        UpdateState const read = parse_update_state("checked 1793000000\r\nlatest 0.2.1\nsomething else\nlatest not-a-version\n");
        CHECK_EQ(read.checked, std::int64_t { 1793000000 });
        CHECK_EQ(read.latest, std::string("0.2.1"));
        CHECK_EQ(serialize_update_state(read), std::string("checked 1793000000\nlatest 0.2.1\n"));
        CHECK_EQ(serialize_update_state(UpdateState { 5, "" }), std::string("checked 5\n"));
        UpdateState const bad = parse_update_state("checked soon\nchecked -4\n");
        CHECK_EQ(bad.checked, std::int64_t { 0 });
    }

    // --- when to ask --------------------------------------------------------------
    {
        std::int64_t const now = 1793000000;
        CHECK(update_check_due(UpdateState {}, now)); // never asked
        CHECK(!update_check_due(UpdateState { now, "" }, now));
        CHECK(!update_check_due(UpdateState { now - update_check_interval + 1, "" }, now));
        CHECK(update_check_due(UpdateState { now - update_check_interval, "" }, now));
        CHECK(update_check_due(UpdateState { now + 500, "" }, now)); // the clock went back
    }

    return sashfold::test::report("update check");
}
