// The ceremony as a state machine: several keys at once (the first touched
// wins, the others are cancelled on the wire), keys plugged in and pulled
// out while it waits, the page's abort and the user's cancel at each
// stage, timeouts, and what the prompt is shown along the way.
#include "AuthCeremony.h"
#include "SoftAuthenticator.h"
#include "Test.h"

#include "auth/Attestation.h"
#include "auth/Client.h"

#include <thread>

using namespace sashfold;
using namespace sashfold::auth;
using sashfold::test::create_request;
using sashfold::test::FakeTransport;
using sashfold::test::get_request;
using sashfold::test::pin_answer;
using sashfold::test::run_ceremony;
using sashfold::test::ScriptedPrompt;
using sashfold::test::SoftOptions;
using sashfold::test::TouchPlan;
using Clock = std::chrono::steady_clock;

namespace {

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void first_touched_wins()
{
    auto transport = std::make_shared<FakeTransport>();
    auto never = transport->plug();
    auto second = transport->plug();
    auto third = transport->plug();
    never->set_touch(TouchPlan::never());
    second->set_touch(TouchPlan::after(250));
    third->set_touch(TouchPlan::never());
    auto prompt = std::make_shared<ScriptedPrompt>();
    auto const outcome = run_ceremony(transport, create_request(), prompt);
    CHECK(outcome.result.has_value());
    if (outcome.result)
        CHECK_EQ(outcome.result->device_path, std::string("soft:2"));
    CHECK_EQ(second->credentials().size(), std::size_t { 1 });
    CHECK(never->credentials().empty());
    CHECK(third->credentials().empty());
    // The other two were told to stop (CTAPHID_CANCEL), not left waiting.
    CHECK_EQ(never->stats().cancels_honored.load(), 1);
    CHECK_EQ(third->stats().cancels_honored.load(), 1);
    CHECK(prompt->saw(PromptState::TouchKey));
    CHECK(prompt->closed);

    // With a PIN: the touch chooses the key first, then the PIN is asked of it alone.
    SoftOptions with_pin;
    with_pin.pin = "1234";
    auto transport2 = std::make_shared<FakeTransport>();
    auto a = transport2->plug(with_pin);
    auto b = transport2->plug(with_pin);
    a->set_touch(TouchPlan::never());
    b->set_touch(TouchPlan::after(150));
    auto request = create_request();
    request.user_verification = Requirement::Required;
    auto pin_prompt = std::make_shared<ScriptedPrompt>();
    pin_prompt->pins = { pin_answer("1234") };
    auto const chosen = run_ceremony(transport2, request, pin_prompt);
    CHECK(chosen.result && chosen.result->device_path == "soft:2");
    CHECK_EQ(pin_prompt->questions.size(), std::size_t { 1 });
    CHECK(pin_prompt->saw(PromptState::TouchKeyAgain));
    CHECK_EQ(a->pin_retries(), 8);
    // Selection, then the PIN, then the touch that makes the credential.
    CHECK_EQ(b->stats().touches.load(), 2);

    // The losing key is cancelled the moment the other is chosen, not when
    // the ceremony ends: by the time the PIN is asked it has heard CANCEL.
    class WatchingPrompt final : public Prompt {
    public:
        explicit WatchingPrompt(std::shared_ptr<test::SoftAuthenticator> loser)
            : m_loser(std::move(loser))
        {
        }
        void show(PromptInfo const&) override { }
        void ask_pin(PinQuestion const&, std::function<void(std::optional<PinAnswer>)> answer) override
        {
            sleep_ms(300);
            cancels_when_asked = m_loser->stats().cancels_honored.load();
            answer(PinAnswer { "1234", {} });
        }
        void ask_account(std::vector<Account> const&, std::function<void(std::optional<std::size_t>)> answer) override { answer(0); }
        void close() override { }
        int cancels_when_asked = -1;

    private:
        std::shared_ptr<test::SoftAuthenticator> m_loser;
    };
    a->stats().cancels_honored = 0;
    auto watching = std::make_shared<WatchingPrompt>(a);
    std::optional<Outcome> watched;
    {
        Ceremony ceremony(request, transport2, watching, [&](Outcome o) { watched = std::move(o); });
        ceremony.start();
        ceremony.wait();
    }
    CHECK(watched && watched->result);
    CHECK_EQ(watching->cancels_when_asked, 1);

    // A key that cannot serve the request and one that can: the user
    // touches the good one.
    SoftOptions u2f;
    u2f.protocol = SoftOptions::Protocol::U2fOnly;
    auto transport3 = std::make_shared<FakeTransport>();
    auto old_key = transport3->plug(u2f);
    auto new_key = transport3->plug();
    old_key->set_touch(TouchPlan::never());
    new_key->set_touch(TouchPlan::after(100));
    auto rk = create_request();
    rk.resident_key = Requirement::Required;
    auto const mixed = run_ceremony(transport3, rk, std::make_shared<ScriptedPrompt>());
    CHECK(mixed.result && mixed.result->device_path == "soft:2");
    // ... and when the unsuitable one is touched, that ends it.
    old_key->set_touch(TouchPlan::after(100));
    new_key->set_touch(TouchPlan::never());
    auto unsuitable_prompt = std::make_shared<ScriptedPrompt>();
    auto const refused = run_ceremony(transport3, rk, unsuitable_prompt);
    CHECK(refused.error && refused.error->reason == Reason::Unsuitable);
    CHECK(unsuitable_prompt->saw(PromptState::KeyUnsuitable));
}

void plugged_and_pulled()
{
    // Nothing plugged in: "insert your key", then a key arrives.
    auto transport = std::make_shared<FakeTransport>();
    auto prompt = std::make_shared<ScriptedPrompt>();
    std::shared_ptr<test::SoftAuthenticator> late;
    auto const outcome = run_ceremony(transport, create_request(), prompt, [&](Ceremony&) {
        sleep_ms(300);
        late = transport->plug();
    });
    CHECK(outcome.result.has_value());
    CHECK(!prompt->states.empty() && prompt->states.front() == PromptState::InsertKey);
    CHECK(prompt->saw(PromptState::TouchKey));
    CHECK(late && late->credentials().size() == 1);

    // The only key is pulled out while it waits for the touch; another is
    // plugged in and touched.
    auto transport2 = std::make_shared<FakeTransport>();
    auto first = transport2->plug();
    first->set_touch(TouchPlan::never());
    auto prompt2 = std::make_shared<ScriptedPrompt>();
    std::shared_ptr<test::SoftAuthenticator> replacement;
    auto const swapped = run_ceremony(transport2, create_request(), prompt2, [&](Ceremony&) {
        sleep_ms(300);
        transport2->unplug(first);
        sleep_ms(300);
        replacement = transport2->plug();
        replacement->set_touch(TouchPlan::after(100));
    });
    CHECK(swapped.result && swapped.result->device_path == "soft:2");
    // After the pull the prompt went back to "insert your key".
    bool insert_after_touch = false;
    bool seen_touch = false;
    for (auto const state : prompt2->states) {
        seen_touch = seen_touch || state == PromptState::TouchKey;
        insert_after_touch = insert_after_touch || (seen_touch && state == PromptState::InsertKey);
    }
    CHECK(insert_after_touch);

    // The chosen key is pulled out while its PIN is being typed: the
    // ceremony fails rather than wait for it.
    SoftOptions with_pin;
    with_pin.pin = "1234";
    auto transport3 = std::make_shared<FakeTransport>();
    auto chosen = transport3->plug(with_pin);
    auto request = create_request();
    request.user_verification = Requirement::Required;
    class PullingPrompt final : public Prompt {
    public:
        PullingPrompt(std::shared_ptr<FakeTransport> t, std::shared_ptr<test::SoftAuthenticator> k)
            : transport(std::move(t))
            , key(std::move(k))
        {
        }
        void show(PromptInfo const&) override { }
        void ask_pin(PinQuestion const&, std::function<void(std::optional<PinAnswer>)> answer) override
        {
            transport->unplug(key);
            answer(PinAnswer { "1234", {} });
        }
        void ask_account(std::vector<Account> const&, std::function<void(std::optional<std::size_t>)> answer) override { answer(0); }
        void close() override { }
        std::shared_ptr<FakeTransport> transport;
        std::shared_ptr<test::SoftAuthenticator> key;
    };
    auto pulling = std::make_shared<PullingPrompt>(transport3, chosen);
    std::optional<Outcome> pulled;
    {
        Ceremony ceremony(request, transport3, pulling, [&](Outcome o) { pulled = std::move(o); });
        auto const start = Clock::now();
        ceremony.start();
        ceremony.wait();
        CHECK(Clock::now() - start < std::chrono::seconds(3));
    }
    CHECK(pulled && pulled->error && pulled->error->reason == Reason::DeviceGone);
}

void cancel_and_timeout()
{
    // The page aborts while the key waits: AbortError, and the key hears CANCEL.
    auto transport = std::make_shared<FakeTransport>();
    auto key = transport->plug();
    key->set_touch(TouchPlan::never());
    auto start = Clock::now();
    auto const aborted = run_ceremony(transport, create_request(), std::make_shared<ScriptedPrompt>(), [](Ceremony& c) {
        sleep_ms(300);
        c.cancel(true);
    });
    CHECK(aborted.error && aborted.error->kind == ErrorKind::Abort && aborted.error->reason == Reason::Aborted);
    CHECK(aborted.error && std::string(aborted.error->dom_exception_name()) == "AbortError");
    CHECK(Clock::now() - start < std::chrono::milliseconds(2000));
    CHECK_EQ(key->stats().cancels_honored.load(), 1);

    // The user closes the prompt: NotAllowedError.
    auto const closed = run_ceremony(transport, create_request(), std::make_shared<ScriptedPrompt>(), [](Ceremony& c) {
        sleep_ms(200);
        c.cancel(false);
    });
    CHECK(closed.error && closed.error->kind == ErrorKind::NotAllowed && closed.error->reason == Reason::Cancelled);

    // Cancelled before any key was found, and with no key at all.
    auto empty = std::make_shared<FakeTransport>();
    auto const nothing = run_ceremony(empty, create_request(), std::make_shared<ScriptedPrompt>(), [](Ceremony& c) { c.cancel(true); });
    CHECK(nothing.error && nothing.error->kind == ErrorKind::Abort);

    // Time runs out with no key, and with a key nobody touches.
    auto quick = create_request();
    quick.timeout = std::chrono::milliseconds(400);
    start = Clock::now();
    auto const no_key = run_ceremony(empty, quick, std::make_shared<ScriptedPrompt>());
    CHECK(no_key.error && no_key.error->reason == Reason::Timeout && no_key.error->kind == ErrorKind::NotAllowed);
    auto const took = Clock::now() - start;
    CHECK(took >= std::chrono::milliseconds(390) && took < std::chrono::milliseconds(1500));
    auto const untouched = run_ceremony(transport, quick, std::make_shared<ScriptedPrompt>());
    CHECK(untouched.error && untouched.error->reason == Reason::Timeout);

    // Aborted while the PIN dialog is open: the dialog's late answer is ignored.
    SoftOptions with_pin;
    with_pin.pin = "1234";
    auto transport2 = std::make_shared<FakeTransport>();
    auto pin_key = transport2->plug(with_pin);
    class SlowPrompt final : public Prompt {
    public:
        void show(PromptInfo const&) override { }
        void ask_pin(PinQuestion const&, std::function<void(std::optional<PinAnswer>)> answer) override
        {
            std::lock_guard lock(mutex);
            pending = std::move(answer);
        }
        void ask_account(std::vector<Account> const&, std::function<void(std::optional<std::size_t>)> answer) override { answer(0); }
        void close() override { }
        std::mutex mutex;
        std::function<void(std::optional<PinAnswer>)> pending;
    };
    auto slow = std::make_shared<SlowPrompt>();
    auto request = create_request();
    request.user_verification = Requirement::Required;
    std::optional<Outcome> outcome;
    {
        Ceremony ceremony(request, transport2, slow, [&](Outcome o) { outcome = std::move(o); });
        ceremony.start();
        sleep_ms(400);
        ceremony.cancel(true);
        ceremony.wait();
    }
    CHECK(outcome && outcome->error && outcome->error->kind == ErrorKind::Abort);
    {
        std::lock_guard lock(slow->mutex);
        CHECK(static_cast<bool>(slow->pending));
        if (slow->pending)
            slow->pending(PinAnswer { "1234", {} }); // after the end: ignored, no crash
    }
    CHECK_EQ(pin_key->pin_retries(), 8);

    // A ceremony destroyed while it runs ends, and reports, on its own.
    std::optional<Outcome> dropped;
    {
        Ceremony ceremony(create_request(), transport, std::make_shared<ScriptedPrompt>(), [&](Outcome o) { dropped = std::move(o); });
        ceremony.start();
        sleep_ms(100);
    }
    CHECK(dropped && dropped->error && dropped->error->reason == Reason::Cancelled);
}

void slow_and_busy_keys()
{
    // A key that reports "processing" before it answers: the prompt hears of it.
    SoftOptions slow;
    slow.processing_ms = 300;
    auto transport = std::make_shared<FakeTransport>();
    transport->plug(slow);
    auto prompt = std::make_shared<ScriptedPrompt>();
    auto const outcome = run_ceremony(transport, create_request(), prompt);
    CHECK(outcome.result.has_value());

    // Another application holds the key (CTAPHID LOCK) for a second: the
    // ceremony waits for it and goes on.
    auto transport2 = std::make_shared<FakeTransport>();
    auto key = transport2->plug();
    DeviceInfo device;
    CtapHidDevice other(key->connect(), device);
    CHECK(other.init(Clock::now() + std::chrono::seconds(1)));
    CHECK(other.lock(1, Clock::now() + std::chrono::seconds(1)));
    auto const waited = run_ceremony(transport2, create_request(), std::make_shared<ScriptedPrompt>());
    CHECK(waited.result.has_value());
    CHECK(key->stats().busy_errors.load() > 0);
}

}

int main()
{
    first_touched_wins();
    plugged_and_pulled();
    cancel_and_timeout();
    slow_and_busy_keys();
    return sashfold::test::report("test_auth_client");
}
