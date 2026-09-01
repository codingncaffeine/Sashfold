#include "html/Tokenizer.h"

#include "core/Ascii.h"
#include "core/Unicode.h"
#include "html/Entities.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>

namespace sashfold::html {

namespace {

// Windows-1252 remapping for numeric references in the C1 range, per the
// numeric-character-reference-end state's table.
struct C1Remap {
    char32_t from;
    char32_t to;
};
constexpr std::array<C1Remap, 27> c1_remap { {
    { 0x80, 0x20AC },
    { 0x82, 0x201A },
    { 0x83, 0x0192 },
    { 0x84, 0x201E },
    { 0x85, 0x2026 },
    { 0x86, 0x2020 },
    { 0x87, 0x2021 },
    { 0x88, 0x02C6 },
    { 0x89, 0x2030 },
    { 0x8A, 0x0160 },
    { 0x8B, 0x2039 },
    { 0x8C, 0x0152 },
    { 0x8E, 0x017D },
    { 0x91, 0x2018 },
    { 0x92, 0x2019 },
    { 0x93, 0x201C },
    { 0x94, 0x201D },
    { 0x95, 0x2022 },
    { 0x96, 0x2013 },
    { 0x97, 0x2014 },
    { 0x98, 0x02DC },
    { 0x99, 0x2122 },
    { 0x9A, 0x0161 },
    { 0x9B, 0x203A },
    { 0x9C, 0x0153 },
    { 0x9E, 0x017E },
    { 0x9F, 0x0178 },
} };

std::vector<std::uint16_t> const& entity_indices_sorted_by_name()
{
    static std::vector<std::uint16_t> const indices = [] {
        std::vector<std::uint16_t> result(std::size(named_entities));
        std::iota(result.begin(), result.end(), std::uint16_t { 0 });
        std::sort(result.begin(), result.end(), [](std::uint16_t a, std::uint16_t b) {
            return named_entities[a].name < named_entities[b].name;
        });
        return result;
    }();
    return indices;
}

NamedEntity const* find_entity_exact(std::string_view name)
{
    auto const& indices = entity_indices_sorted_by_name();
    auto const it = std::lower_bound(indices.begin(), indices.end(), name,
        [](std::uint16_t index, std::string_view value) { return named_entities[index].name < value; });
    if (it == indices.end() || named_entities[*it].name != name)
        return nullptr;
    return &named_entities[*it];
}

bool is_ascii_whitespace(char32_t c)
{
    return c == U'\t' || c == U'\n' || c == U'\f' || c == U'\r' || c == U' ';
}

}

Tokenizer::Tokenizer(std::string_view utf8_input)
    : m_input(InputStream::from_utf8(utf8_input))
{
}

Tokenizer::Tokenizer(InputStream input, State initial_state, std::string last_start_tag_name)
    : m_input(std::move(input))
    , m_state(initial_state)
    , m_last_start_tag_name(std::move(last_start_tag_name))
{
}

std::optional<Tokenizer::State> Tokenizer::state_from_test_name(std::string_view name)
{
    if (name == "Data state")
        return State::Data;
    if (name == "PLAINTEXT state")
        return State::PLAINTEXT;
    if (name == "RCDATA state")
        return State::RCDATA;
    if (name == "RAWTEXT state")
        return State::RAWTEXT;
    if (name == "Script data state")
        return State::ScriptData;
    if (name == "CDATA section state")
        return State::CDATASection;
    return std::nullopt;
}

std::optional<Token> Tokenizer::next_token()
{
    while (m_queue.empty() && !m_eof_emitted)
        step();
    if (m_queue.empty())
        return std::nullopt;
    Token token = std::move(m_queue.front());
    m_queue.pop_front();
    return token;
}

void Tokenizer::step()
{
    // A few states operate on lookahead and must run before any consume.
    switch (m_state) {
    case State::MarkupDeclarationOpen:
        handle_markup_declaration_open();
        return;
    case State::NamedCharacterReference:
        handle_named_character_reference();
        return;
    case State::NumericCharacterReferenceEnd:
        handle_numeric_character_reference_end();
        return;
    default:
        break;
    }

    std::optional<char32_t> c;
    if (m_reconsume) {
        c = m_current;
        m_reconsume = false;
    } else {
        c = m_input.next();
        m_current = c;
    }
    dispatch(c);
}

void Tokenizer::dispatch(std::optional<char32_t> c)
{
    switch (m_state) {
    case State::Data: handle_data(c); break;
    case State::RCDATA: handle_rcdata(c); break;
    case State::RAWTEXT: handle_rawtext(c); break;
    case State::ScriptData: handle_script_data(c); break;
    case State::PLAINTEXT: handle_plaintext(c); break;
    case State::TagOpen: handle_tag_open(c); break;
    case State::EndTagOpen: handle_end_tag_open(c); break;
    case State::TagName: handle_tag_name(c); break;
    case State::RCDATALessThanSign: handle_less_than_sign(c, State::RCDATAEndTagOpen, State::RCDATA, false); break;
    case State::RCDATAEndTagOpen: handle_text_end_tag_open(c, State::RCDATAEndTagName, State::RCDATA); break;
    case State::RCDATAEndTagName: handle_text_end_tag_name(c, State::RCDATA); break;
    case State::RAWTEXTLessThanSign: handle_less_than_sign(c, State::RAWTEXTEndTagOpen, State::RAWTEXT, false); break;
    case State::RAWTEXTEndTagOpen: handle_text_end_tag_open(c, State::RAWTEXTEndTagName, State::RAWTEXT); break;
    case State::RAWTEXTEndTagName: handle_text_end_tag_name(c, State::RAWTEXT); break;
    case State::ScriptDataLessThanSign: handle_less_than_sign(c, State::ScriptDataEndTagOpen, State::ScriptData, true); break;
    case State::ScriptDataEndTagOpen: handle_text_end_tag_open(c, State::ScriptDataEndTagName, State::ScriptData); break;
    case State::ScriptDataEndTagName: handle_text_end_tag_name(c, State::ScriptData); break;
    case State::ScriptDataEscapeStart: handle_script_data_escape_start(c); break;
    case State::ScriptDataEscapeStartDash: handle_script_data_escape_start_dash(c); break;
    case State::ScriptDataEscaped: handle_script_data_escaped(c); break;
    case State::ScriptDataEscapedDash: handle_script_data_escaped_dash(c); break;
    case State::ScriptDataEscapedDashDash: handle_script_data_escaped_dash_dash(c); break;
    case State::ScriptDataEscapedLessThanSign: handle_script_data_escaped_less_than_sign(c); break;
    case State::ScriptDataEscapedEndTagOpen: handle_text_end_tag_open(c, State::ScriptDataEscapedEndTagName, State::ScriptDataEscaped); break;
    case State::ScriptDataEscapedEndTagName: handle_text_end_tag_name(c, State::ScriptDataEscaped); break;
    case State::ScriptDataDoubleEscapeStart:
        handle_script_data_double_escape_start_or_end(c, State::ScriptDataDoubleEscaped, State::ScriptDataEscaped);
        break;
    case State::ScriptDataDoubleEscaped: handle_script_data_double_escaped(c); break;
    case State::ScriptDataDoubleEscapedDash: handle_script_data_double_escaped_dash(c); break;
    case State::ScriptDataDoubleEscapedDashDash: handle_script_data_double_escaped_dash_dash(c); break;
    case State::ScriptDataDoubleEscapedLessThanSign: handle_script_data_double_escaped_less_than_sign(c); break;
    case State::ScriptDataDoubleEscapeEnd:
        handle_script_data_double_escape_start_or_end(c, State::ScriptDataEscaped, State::ScriptDataDoubleEscaped);
        break;
    case State::BeforeAttributeName: handle_before_attribute_name(c); break;
    case State::AttributeName: handle_attribute_name(c); break;
    case State::AfterAttributeName: handle_after_attribute_name(c); break;
    case State::BeforeAttributeValue: handle_before_attribute_value(c); break;
    case State::AttributeValueDoubleQuoted: handle_attribute_value_quoted(c, U'"'); break;
    case State::AttributeValueSingleQuoted: handle_attribute_value_quoted(c, U'\''); break;
    case State::AttributeValueUnquoted: handle_attribute_value_unquoted(c); break;
    case State::AfterAttributeValueQuoted: handle_after_attribute_value_quoted(c); break;
    case State::SelfClosingStartTag: handle_self_closing_start_tag(c); break;
    case State::BogusComment: handle_bogus_comment(c); break;
    case State::CommentStart: handle_comment_start(c); break;
    case State::CommentStartDash: handle_comment_start_dash(c); break;
    case State::Comment: handle_comment(c); break;
    case State::CommentLessThanSign: handle_comment_less_than_sign(c); break;
    case State::CommentLessThanSignBang: handle_comment_less_than_sign_bang(c); break;
    case State::CommentLessThanSignBangDash: handle_comment_less_than_sign_bang_dash(c); break;
    case State::CommentLessThanSignBangDashDash: handle_comment_less_than_sign_bang_dash_dash(c); break;
    case State::CommentEndDash: handle_comment_end_dash(c); break;
    case State::CommentEnd: handle_comment_end(c); break;
    case State::CommentEndBang: handle_comment_end_bang(c); break;
    case State::DOCTYPE: handle_doctype(c); break;
    case State::BeforeDOCTYPEName: handle_before_doctype_name(c); break;
    case State::DOCTYPEName: handle_doctype_name(c); break;
    case State::AfterDOCTYPEName: handle_after_doctype_name(c); break;
    case State::AfterDOCTYPEPublicKeyword: handle_after_doctype_public_keyword(c); break;
    case State::BeforeDOCTYPEPublicIdentifier: handle_before_doctype_public_identifier(c); break;
    case State::DOCTYPEPublicIdentifierDoubleQuoted: handle_doctype_public_identifier_quoted(c, U'"'); break;
    case State::DOCTYPEPublicIdentifierSingleQuoted: handle_doctype_public_identifier_quoted(c, U'\''); break;
    case State::AfterDOCTYPEPublicIdentifier: handle_after_doctype_public_identifier(c); break;
    case State::BetweenDOCTYPEPublicAndSystemIdentifiers: handle_between_doctype_public_and_system(c); break;
    case State::AfterDOCTYPESystemKeyword: handle_after_doctype_system_keyword(c); break;
    case State::BeforeDOCTYPESystemIdentifier: handle_before_doctype_system_identifier(c); break;
    case State::DOCTYPESystemIdentifierDoubleQuoted: handle_doctype_system_identifier_quoted(c, U'"'); break;
    case State::DOCTYPESystemIdentifierSingleQuoted: handle_doctype_system_identifier_quoted(c, U'\''); break;
    case State::AfterDOCTYPESystemIdentifier: handle_after_doctype_system_identifier(c); break;
    case State::BogusDOCTYPE: handle_bogus_doctype(c); break;
    case State::CDATASection: handle_cdata_section(c); break;
    case State::CDATASectionBracket: handle_cdata_section_bracket(c); break;
    case State::CDATASectionEnd: handle_cdata_section_end(c); break;
    case State::CharacterReference: handle_character_reference(c); break;
    case State::AmbiguousAmpersand: handle_ambiguous_ampersand(c); break;
    case State::NumericCharacterReference: handle_numeric_character_reference(c); break;
    case State::HexadecimalCharacterReferenceStart: handle_hexadecimal_start(c); break;
    case State::DecimalCharacterReferenceStart: handle_decimal_start(c); break;
    case State::HexadecimalCharacterReference: handle_hexadecimal(c); break;
    case State::DecimalCharacterReference: handle_decimal(c); break;
    case State::MarkupDeclarationOpen:
    case State::NamedCharacterReference:
    case State::NumericCharacterReferenceEnd:
        // Lookahead states are serviced in step() before any consume; reaching
        // here is an internal fault. Recover rather than lie about it.
        error("internal-lookahead-state-dispatched");
        switch_to(State::Data);
        break;
    }
}

// --- Emission and bookkeeping -----------------------------------------------

void Tokenizer::error(std::string_view code)
{
    m_errors.push_back(ParseError { std::string(code), m_input.position() });
}

void Tokenizer::emit_character(char32_t c)
{
    Token token;
    token.type = Token::Type::Character;
    token.code_point = c;
    m_queue.push_back(std::move(token));
}

void Tokenizer::emit_temp_buffer_as_characters()
{
    for (char32_t c : m_temp_buffer)
        emit_character(c);
}

void Tokenizer::emit_eof()
{
    Token token;
    token.type = Token::Type::EndOfFile;
    m_queue.push_back(std::move(token));
    m_eof_emitted = true;
}

void Tokenizer::create_start_tag()
{
    m_current_token = Token {};
    m_current_token.type = Token::Type::StartTag;
    m_has_current_attribute = false;
}

void Tokenizer::create_end_tag()
{
    m_current_token = Token {};
    m_current_token.type = Token::Type::EndTag;
    m_has_current_attribute = false;
}

void Tokenizer::create_comment(std::string initial_data)
{
    m_current_token = Token {};
    m_current_token.type = Token::Type::Comment;
    m_current_token.data = std::move(initial_data);
}

void Tokenizer::create_doctype()
{
    m_current_token = Token {};
    m_current_token.type = Token::Type::Doctype;
}

void Tokenizer::emit_current_tag()
{
    commit_current_attribute();
    if (m_current_token.type == Token::Type::EndTag) {
        if (!m_current_token.attributes.empty()) {
            error("end-tag-with-attributes");
            m_current_token.attributes.clear();
        }
        if (m_current_token.self_closing) {
            error("end-tag-with-trailing-solidus");
            m_current_token.self_closing = false;
        }
    } else {
        m_last_start_tag_name = m_current_token.tag_name;
    }
    m_queue.push_back(std::move(m_current_token));
    m_current_token = Token {};
}

void Tokenizer::emit_comment()
{
    m_queue.push_back(std::move(m_current_token));
    m_current_token = Token {};
}

void Tokenizer::emit_doctype()
{
    m_queue.push_back(std::move(m_current_token));
    m_current_token = Token {};
}

void Tokenizer::start_new_attribute()
{
    commit_current_attribute();
    m_current_token.attributes.push_back(Attribute {});
    m_has_current_attribute = true;
    m_current_attribute_ignored = false;
}

void Tokenizer::commit_current_attribute()
{
    if (m_has_current_attribute && m_current_attribute_ignored)
        m_current_token.attributes.pop_back();
    m_has_current_attribute = false;
    m_current_attribute_ignored = false;
}

void Tokenizer::finish_attribute_name()
{
    if (!m_has_current_attribute || m_current_attribute_ignored)
        return;
    auto const& attributes = m_current_token.attributes;
    std::string const& name = attributes.back().name;
    for (std::size_t i = 0; i + 1 < attributes.size(); ++i) {
        if (attributes[i].name == name) {
            error("duplicate-attribute");
            m_current_attribute_ignored = true;
            return;
        }
    }
}

void Tokenizer::append_to_attribute_value(char32_t c)
{
    if (!m_has_current_attribute)
        return;
    append_utf8(m_current_token.attributes.back().value, c);
}

bool Tokenizer::current_end_tag_is_appropriate() const
{
    return !m_last_start_tag_name.empty() && m_current_token.tag_name == m_last_start_tag_name;
}

void Tokenizer::start_character_reference(State return_state)
{
    m_return_state = return_state;
    m_temp_buffer = U"&";
    switch_to(State::CharacterReference);
}

bool Tokenizer::character_reference_in_attribute() const
{
    return m_return_state == State::AttributeValueDoubleQuoted
        || m_return_state == State::AttributeValueSingleQuoted
        || m_return_state == State::AttributeValueUnquoted;
}

void Tokenizer::flush_code_points_consumed_as_character_reference()
{
    for (char32_t c : m_temp_buffer) {
        if (character_reference_in_attribute())
            append_to_attribute_value(c);
        else
            emit_character(c);
    }
}

// --- Text states -------------------------------------------------------------

void Tokenizer::handle_data(std::optional<char32_t> c)
{
    if (!c) {
        emit_eof();
        return;
    }
    switch (*c) {
    case U'&':
        start_character_reference(State::Data);
        break;
    case U'<':
        switch_to(State::TagOpen);
        break;
    case U'\0':
        error("unexpected-null-character");
        emit_character(*c);
        break;
    default:
        emit_character(*c);
        break;
    }
}

void Tokenizer::handle_rcdata(std::optional<char32_t> c)
{
    if (!c) {
        emit_eof();
        return;
    }
    switch (*c) {
    case U'&':
        start_character_reference(State::RCDATA);
        break;
    case U'<':
        switch_to(State::RCDATALessThanSign);
        break;
    case U'\0':
        error("unexpected-null-character");
        emit_character(replacement_character);
        break;
    default:
        emit_character(*c);
        break;
    }
}

void Tokenizer::handle_rawtext(std::optional<char32_t> c)
{
    if (!c) {
        emit_eof();
        return;
    }
    switch (*c) {
    case U'<':
        switch_to(State::RAWTEXTLessThanSign);
        break;
    case U'\0':
        error("unexpected-null-character");
        emit_character(replacement_character);
        break;
    default:
        emit_character(*c);
        break;
    }
}

void Tokenizer::handle_script_data(std::optional<char32_t> c)
{
    if (!c) {
        emit_eof();
        return;
    }
    switch (*c) {
    case U'<':
        switch_to(State::ScriptDataLessThanSign);
        break;
    case U'\0':
        error("unexpected-null-character");
        emit_character(replacement_character);
        break;
    default:
        emit_character(*c);
        break;
    }
}

void Tokenizer::handle_plaintext(std::optional<char32_t> c)
{
    if (!c) {
        emit_eof();
        return;
    }
    if (*c == U'\0') {
        error("unexpected-null-character");
        emit_character(replacement_character);
        return;
    }
    emit_character(*c);
}

// --- Tag states --------------------------------------------------------------

void Tokenizer::handle_tag_open(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-before-tag-name");
        emit_character(U'<');
        emit_eof();
        return;
    }
    if (*c == U'!') {
        switch_to(State::MarkupDeclarationOpen);
        return;
    }
    if (*c == U'/') {
        switch_to(State::EndTagOpen);
        return;
    }
    if (is_ascii_alpha(*c)) {
        create_start_tag();
        reconsume_in(State::TagName);
        return;
    }
    if (*c == U'?') {
        error("unexpected-question-mark-instead-of-tag-name");
        create_comment();
        reconsume_in(State::BogusComment);
        return;
    }
    error("invalid-first-character-of-tag-name");
    emit_character(U'<');
    reconsume_in(State::Data);
}

void Tokenizer::handle_end_tag_open(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-before-tag-name");
        emit_character(U'<');
        emit_character(U'/');
        emit_eof();
        return;
    }
    if (is_ascii_alpha(*c)) {
        create_end_tag();
        reconsume_in(State::TagName);
        return;
    }
    if (*c == U'>') {
        error("missing-end-tag-name");
        switch_to(State::Data);
        return;
    }
    error("invalid-first-character-of-tag-name");
    create_comment();
    reconsume_in(State::BogusComment);
}

void Tokenizer::handle_tag_name(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-tag");
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c)) {
        switch_to(State::BeforeAttributeName);
        return;
    }
    if (*c == U'/') {
        switch_to(State::SelfClosingStartTag);
        return;
    }
    if (*c == U'>') {
        switch_to(State::Data);
        emit_current_tag();
        return;
    }
    if (*c == U'\0') {
        error("unexpected-null-character");
        append_utf8(m_current_token.tag_name, replacement_character);
        return;
    }
    append_utf8(m_current_token.tag_name, to_ascii_lowercase(*c));
}

// --- RCDATA / RAWTEXT / script-data end-tag machinery ------------------------

void Tokenizer::handle_less_than_sign(std::optional<char32_t> c, State end_tag_open_state, State return_text_state, bool script_bang)
{
    if (c && *c == U'/') {
        m_temp_buffer.clear();
        switch_to(end_tag_open_state);
        return;
    }
    if (script_bang && c && *c == U'!') {
        switch_to(State::ScriptDataEscapeStart);
        emit_character(U'<');
        emit_character(U'!');
        return;
    }
    emit_character(U'<');
    reconsume_in(return_text_state);
}

void Tokenizer::handle_text_end_tag_open(std::optional<char32_t> c, State end_tag_name_state, State return_text_state)
{
    if (c && is_ascii_alpha(*c)) {
        create_end_tag();
        reconsume_in(end_tag_name_state);
        return;
    }
    emit_character(U'<');
    emit_character(U'/');
    reconsume_in(return_text_state);
}

void Tokenizer::handle_text_end_tag_name(std::optional<char32_t> c, State return_text_state)
{
    if (c) {
        if (is_tokenizer_whitespace(*c) && current_end_tag_is_appropriate()) {
            switch_to(State::BeforeAttributeName);
            return;
        }
        if (*c == U'/' && current_end_tag_is_appropriate()) {
            switch_to(State::SelfClosingStartTag);
            return;
        }
        if (*c == U'>' && current_end_tag_is_appropriate()) {
            switch_to(State::Data);
            emit_current_tag();
            return;
        }
        if (is_ascii_alpha(*c)) {
            append_utf8(m_current_token.tag_name, to_ascii_lowercase(*c));
            m_temp_buffer.push_back(*c);
            return;
        }
    }
    emit_character(U'<');
    emit_character(U'/');
    emit_temp_buffer_as_characters();
    reconsume_in(return_text_state);
}

// --- Script-data escaping ----------------------------------------------------

void Tokenizer::handle_script_data_escape_start(std::optional<char32_t> c)
{
    if (c && *c == U'-') {
        switch_to(State::ScriptDataEscapeStartDash);
        emit_character(U'-');
        return;
    }
    reconsume_in(State::ScriptData);
}

void Tokenizer::handle_script_data_escape_start_dash(std::optional<char32_t> c)
{
    if (c && *c == U'-') {
        switch_to(State::ScriptDataEscapedDashDash);
        emit_character(U'-');
        return;
    }
    reconsume_in(State::ScriptData);
}

void Tokenizer::handle_script_data_escaped(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-script-html-comment-like-text");
        emit_eof();
        return;
    }
    switch (*c) {
    case U'-':
        switch_to(State::ScriptDataEscapedDash);
        emit_character(U'-');
        break;
    case U'<':
        switch_to(State::ScriptDataEscapedLessThanSign);
        break;
    case U'\0':
        error("unexpected-null-character");
        emit_character(replacement_character);
        break;
    default:
        emit_character(*c);
        break;
    }
}

void Tokenizer::handle_script_data_escaped_dash(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-script-html-comment-like-text");
        emit_eof();
        return;
    }
    switch (*c) {
    case U'-':
        switch_to(State::ScriptDataEscapedDashDash);
        emit_character(U'-');
        break;
    case U'<':
        switch_to(State::ScriptDataEscapedLessThanSign);
        break;
    case U'\0':
        error("unexpected-null-character");
        switch_to(State::ScriptDataEscaped);
        emit_character(replacement_character);
        break;
    default:
        switch_to(State::ScriptDataEscaped);
        emit_character(*c);
        break;
    }
}

void Tokenizer::handle_script_data_escaped_dash_dash(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-script-html-comment-like-text");
        emit_eof();
        return;
    }
    switch (*c) {
    case U'-':
        emit_character(U'-');
        break;
    case U'<':
        switch_to(State::ScriptDataEscapedLessThanSign);
        break;
    case U'>':
        switch_to(State::ScriptData);
        emit_character(U'>');
        break;
    case U'\0':
        error("unexpected-null-character");
        switch_to(State::ScriptDataEscaped);
        emit_character(replacement_character);
        break;
    default:
        switch_to(State::ScriptDataEscaped);
        emit_character(*c);
        break;
    }
}

void Tokenizer::handle_script_data_escaped_less_than_sign(std::optional<char32_t> c)
{
    if (c && *c == U'/') {
        m_temp_buffer.clear();
        switch_to(State::ScriptDataEscapedEndTagOpen);
        return;
    }
    if (c && is_ascii_alpha(*c)) {
        m_temp_buffer.clear();
        emit_character(U'<');
        reconsume_in(State::ScriptDataDoubleEscapeStart);
        return;
    }
    emit_character(U'<');
    reconsume_in(State::ScriptDataEscaped);
}

void Tokenizer::handle_script_data_double_escape_start_or_end(std::optional<char32_t> c, State on_match, State on_mismatch)
{
    if (c && (is_tokenizer_whitespace(*c) || *c == U'/' || *c == U'>')) {
        switch_to(m_temp_buffer == U"script" ? on_match : on_mismatch);
        emit_character(*c);
        return;
    }
    if (c && is_ascii_alpha(*c)) {
        m_temp_buffer.push_back(to_ascii_lowercase(*c));
        emit_character(*c);
        return;
    }
    reconsume_in(on_mismatch);
}

void Tokenizer::handle_script_data_double_escaped(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-script-html-comment-like-text");
        emit_eof();
        return;
    }
    switch (*c) {
    case U'-':
        switch_to(State::ScriptDataDoubleEscapedDash);
        emit_character(U'-');
        break;
    case U'<':
        switch_to(State::ScriptDataDoubleEscapedLessThanSign);
        emit_character(U'<');
        break;
    case U'\0':
        error("unexpected-null-character");
        emit_character(replacement_character);
        break;
    default:
        emit_character(*c);
        break;
    }
}

void Tokenizer::handle_script_data_double_escaped_dash(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-script-html-comment-like-text");
        emit_eof();
        return;
    }
    switch (*c) {
    case U'-':
        switch_to(State::ScriptDataDoubleEscapedDashDash);
        emit_character(U'-');
        break;
    case U'<':
        switch_to(State::ScriptDataDoubleEscapedLessThanSign);
        emit_character(U'<');
        break;
    case U'\0':
        error("unexpected-null-character");
        switch_to(State::ScriptDataDoubleEscaped);
        emit_character(replacement_character);
        break;
    default:
        switch_to(State::ScriptDataDoubleEscaped);
        emit_character(*c);
        break;
    }
}

void Tokenizer::handle_script_data_double_escaped_dash_dash(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-script-html-comment-like-text");
        emit_eof();
        return;
    }
    switch (*c) {
    case U'-':
        emit_character(U'-');
        break;
    case U'<':
        switch_to(State::ScriptDataDoubleEscapedLessThanSign);
        emit_character(U'<');
        break;
    case U'>':
        switch_to(State::ScriptData);
        emit_character(U'>');
        break;
    case U'\0':
        error("unexpected-null-character");
        switch_to(State::ScriptDataDoubleEscaped);
        emit_character(replacement_character);
        break;
    default:
        switch_to(State::ScriptDataDoubleEscaped);
        emit_character(*c);
        break;
    }
}

void Tokenizer::handle_script_data_double_escaped_less_than_sign(std::optional<char32_t> c)
{
    if (c && *c == U'/') {
        m_temp_buffer.clear();
        switch_to(State::ScriptDataDoubleEscapeEnd);
        emit_character(U'/');
        return;
    }
    reconsume_in(State::ScriptDataDoubleEscaped);
}

// --- Attribute states --------------------------------------------------------

void Tokenizer::handle_before_attribute_name(std::optional<char32_t> c)
{
    if (!c || *c == U'/' || *c == U'>') {
        reconsume_in(State::AfterAttributeName);
        return;
    }
    if (is_tokenizer_whitespace(*c))
        return;
    if (*c == U'=') {
        error("unexpected-equals-sign-before-attribute-name");
        start_new_attribute();
        append_utf8(m_current_token.attributes.back().name, *c);
        switch_to(State::AttributeName);
        return;
    }
    start_new_attribute();
    reconsume_in(State::AttributeName);
}

void Tokenizer::handle_attribute_name(std::optional<char32_t> c)
{
    if (!c || is_tokenizer_whitespace(*c) || *c == U'/' || *c == U'>') {
        finish_attribute_name();
        reconsume_in(State::AfterAttributeName);
        return;
    }
    if (*c == U'=') {
        finish_attribute_name();
        switch_to(State::BeforeAttributeValue);
        return;
    }
    if (*c == U'\0') {
        error("unexpected-null-character");
        append_utf8(m_current_token.attributes.back().name, replacement_character);
        return;
    }
    if (*c == U'"' || *c == U'\'' || *c == U'<')
        error("unexpected-character-in-attribute-name");
    append_utf8(m_current_token.attributes.back().name, to_ascii_lowercase(*c));
}

void Tokenizer::handle_after_attribute_name(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-tag");
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c))
        return;
    if (*c == U'/') {
        switch_to(State::SelfClosingStartTag);
        return;
    }
    if (*c == U'=') {
        switch_to(State::BeforeAttributeValue);
        return;
    }
    if (*c == U'>') {
        switch_to(State::Data);
        emit_current_tag();
        return;
    }
    start_new_attribute();
    reconsume_in(State::AttributeName);
}

void Tokenizer::handle_before_attribute_value(std::optional<char32_t> c)
{
    if (c && is_tokenizer_whitespace(*c))
        return;
    if (c && *c == U'"') {
        switch_to(State::AttributeValueDoubleQuoted);
        return;
    }
    if (c && *c == U'\'') {
        switch_to(State::AttributeValueSingleQuoted);
        return;
    }
    if (c && *c == U'>') {
        error("missing-attribute-value");
        switch_to(State::Data);
        emit_current_tag();
        return;
    }
    reconsume_in(State::AttributeValueUnquoted);
}

void Tokenizer::handle_attribute_value_quoted(std::optional<char32_t> c, char32_t quote)
{
    if (!c) {
        error("eof-in-tag");
        emit_eof();
        return;
    }
    if (*c == quote) {
        switch_to(State::AfterAttributeValueQuoted);
        return;
    }
    if (*c == U'&') {
        start_character_reference(quote == U'"' ? State::AttributeValueDoubleQuoted : State::AttributeValueSingleQuoted);
        return;
    }
    if (*c == U'\0') {
        error("unexpected-null-character");
        append_to_attribute_value(replacement_character);
        return;
    }
    append_to_attribute_value(*c);
}

void Tokenizer::handle_attribute_value_unquoted(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-tag");
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c)) {
        switch_to(State::BeforeAttributeName);
        return;
    }
    if (*c == U'&') {
        start_character_reference(State::AttributeValueUnquoted);
        return;
    }
    if (*c == U'>') {
        switch_to(State::Data);
        emit_current_tag();
        return;
    }
    if (*c == U'\0') {
        error("unexpected-null-character");
        append_to_attribute_value(replacement_character);
        return;
    }
    if (*c == U'"' || *c == U'\'' || *c == U'<' || *c == U'=' || *c == U'`')
        error("unexpected-character-in-unquoted-attribute-value");
    append_to_attribute_value(*c);
}

void Tokenizer::handle_after_attribute_value_quoted(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-tag");
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c)) {
        switch_to(State::BeforeAttributeName);
        return;
    }
    if (*c == U'/') {
        switch_to(State::SelfClosingStartTag);
        return;
    }
    if (*c == U'>') {
        switch_to(State::Data);
        emit_current_tag();
        return;
    }
    error("missing-whitespace-between-attributes");
    reconsume_in(State::BeforeAttributeName);
}

void Tokenizer::handle_self_closing_start_tag(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-tag");
        emit_eof();
        return;
    }
    if (*c == U'>') {
        m_current_token.self_closing = true;
        switch_to(State::Data);
        emit_current_tag();
        return;
    }
    error("unexpected-solidus-in-tag");
    reconsume_in(State::BeforeAttributeName);
}

// --- Comments ----------------------------------------------------------------

void Tokenizer::handle_bogus_comment(std::optional<char32_t> c)
{
    if (!c) {
        emit_comment();
        emit_eof();
        return;
    }
    if (*c == U'>') {
        switch_to(State::Data);
        emit_comment();
        return;
    }
    if (*c == U'\0') {
        error("unexpected-null-character");
        append_utf8(m_current_token.data, replacement_character);
        return;
    }
    append_utf8(m_current_token.data, *c);
}

void Tokenizer::handle_markup_declaration_open()
{
    if (m_input.has(1) && m_input.peek(0) == U'-' && m_input.peek(1) == U'-') {
        m_input.advance(2);
        create_comment();
        switch_to(State::CommentStart);
        return;
    }
    if (m_input.lookahead_equals_ignoring_case(U"doctype")) {
        m_input.advance(7);
        switch_to(State::DOCTYPE);
        return;
    }
    if (m_input.remaining().starts_with(U"[CDATA[")) {
        m_input.advance(7);
        if (m_in_foreign_content) {
            switch_to(State::CDATASection);
        } else {
            error("cdata-in-html-content");
            create_comment("[CDATA[");
            switch_to(State::BogusComment);
        }
        return;
    }
    error("incorrectly-opened-comment");
    create_comment();
    switch_to(State::BogusComment); // consumes nothing further in this state
}

void Tokenizer::handle_comment_start(std::optional<char32_t> c)
{
    if (c && *c == U'-') {
        switch_to(State::CommentStartDash);
        return;
    }
    if (c && *c == U'>') {
        error("abrupt-closing-of-empty-comment");
        switch_to(State::Data);
        emit_comment();
        return;
    }
    reconsume_in(State::Comment);
}

void Tokenizer::handle_comment_start_dash(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-comment");
        emit_comment();
        emit_eof();
        return;
    }
    if (*c == U'-') {
        switch_to(State::CommentEnd);
        return;
    }
    if (*c == U'>') {
        error("abrupt-closing-of-empty-comment");
        switch_to(State::Data);
        emit_comment();
        return;
    }
    m_current_token.data.push_back('-');
    reconsume_in(State::Comment);
}

void Tokenizer::handle_comment(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-comment");
        emit_comment();
        emit_eof();
        return;
    }
    switch (*c) {
    case U'<':
        append_utf8(m_current_token.data, *c);
        switch_to(State::CommentLessThanSign);
        break;
    case U'-':
        switch_to(State::CommentEndDash);
        break;
    case U'\0':
        error("unexpected-null-character");
        append_utf8(m_current_token.data, replacement_character);
        break;
    default:
        append_utf8(m_current_token.data, *c);
        break;
    }
}

void Tokenizer::handle_comment_less_than_sign(std::optional<char32_t> c)
{
    if (c && *c == U'!') {
        append_utf8(m_current_token.data, *c);
        switch_to(State::CommentLessThanSignBang);
        return;
    }
    if (c && *c == U'<') {
        append_utf8(m_current_token.data, *c);
        return;
    }
    reconsume_in(State::Comment);
}

void Tokenizer::handle_comment_less_than_sign_bang(std::optional<char32_t> c)
{
    if (c && *c == U'-') {
        switch_to(State::CommentLessThanSignBangDash);
        return;
    }
    reconsume_in(State::Comment);
}

void Tokenizer::handle_comment_less_than_sign_bang_dash(std::optional<char32_t> c)
{
    if (c && *c == U'-') {
        switch_to(State::CommentLessThanSignBangDashDash);
        return;
    }
    reconsume_in(State::CommentEndDash);
}

void Tokenizer::handle_comment_less_than_sign_bang_dash_dash(std::optional<char32_t> c)
{
    if (!c || *c == U'>') {
        reconsume_in(State::CommentEnd);
        return;
    }
    error("nested-comment");
    reconsume_in(State::CommentEnd);
}

void Tokenizer::handle_comment_end_dash(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-comment");
        emit_comment();
        emit_eof();
        return;
    }
    if (*c == U'-') {
        switch_to(State::CommentEnd);
        return;
    }
    m_current_token.data.push_back('-');
    reconsume_in(State::Comment);
}

void Tokenizer::handle_comment_end(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-comment");
        emit_comment();
        emit_eof();
        return;
    }
    if (*c == U'>') {
        switch_to(State::Data);
        emit_comment();
        return;
    }
    if (*c == U'!') {
        switch_to(State::CommentEndBang);
        return;
    }
    if (*c == U'-') {
        m_current_token.data.push_back('-');
        return;
    }
    m_current_token.data.append("--");
    reconsume_in(State::Comment);
}

void Tokenizer::handle_comment_end_bang(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-comment");
        emit_comment();
        emit_eof();
        return;
    }
    if (*c == U'-') {
        m_current_token.data.append("--!");
        switch_to(State::CommentEndDash);
        return;
    }
    if (*c == U'>') {
        error("incorrectly-closed-comment");
        switch_to(State::Data);
        emit_comment();
        return;
    }
    m_current_token.data.append("--!");
    reconsume_in(State::Comment);
}

// --- DOCTYPE -----------------------------------------------------------------

void Tokenizer::handle_doctype(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-doctype");
        create_doctype();
        m_current_token.force_quirks = true;
        emit_doctype();
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c)) {
        switch_to(State::BeforeDOCTYPEName);
        return;
    }
    if (*c == U'>') {
        reconsume_in(State::BeforeDOCTYPEName);
        return;
    }
    error("missing-whitespace-before-doctype-name");
    reconsume_in(State::BeforeDOCTYPEName);
}

void Tokenizer::handle_before_doctype_name(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-doctype");
        create_doctype();
        m_current_token.force_quirks = true;
        emit_doctype();
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c))
        return;
    if (*c == U'>') {
        error("missing-doctype-name");
        create_doctype();
        m_current_token.force_quirks = true;
        switch_to(State::Data);
        emit_doctype();
        return;
    }
    create_doctype();
    m_current_token.doctype_name = std::string {};
    if (*c == U'\0') {
        error("unexpected-null-character");
        append_utf8(*m_current_token.doctype_name, replacement_character);
    } else {
        append_utf8(*m_current_token.doctype_name, to_ascii_lowercase(*c));
    }
    switch_to(State::DOCTYPEName);
}

void Tokenizer::handle_doctype_name(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-doctype");
        m_current_token.force_quirks = true;
        emit_doctype();
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c)) {
        switch_to(State::AfterDOCTYPEName);
        return;
    }
    if (*c == U'>') {
        switch_to(State::Data);
        emit_doctype();
        return;
    }
    if (*c == U'\0') {
        error("unexpected-null-character");
        append_utf8(*m_current_token.doctype_name, replacement_character);
        return;
    }
    append_utf8(*m_current_token.doctype_name, to_ascii_lowercase(*c));
}

void Tokenizer::handle_after_doctype_name(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-doctype");
        m_current_token.force_quirks = true;
        emit_doctype();
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c))
        return;
    if (*c == U'>') {
        switch_to(State::Data);
        emit_doctype();
        return;
    }
    if (to_ascii_lowercase(*c) == U'p' && m_input.lookahead_equals_ignoring_case(U"ublic")) {
        m_input.advance(5);
        switch_to(State::AfterDOCTYPEPublicKeyword);
        return;
    }
    if (to_ascii_lowercase(*c) == U's' && m_input.lookahead_equals_ignoring_case(U"ystem")) {
        m_input.advance(5);
        switch_to(State::AfterDOCTYPESystemKeyword);
        return;
    }
    error("invalid-character-sequence-after-doctype-name");
    m_current_token.force_quirks = true;
    reconsume_in(State::BogusDOCTYPE);
}

void Tokenizer::handle_after_doctype_public_keyword(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-doctype");
        m_current_token.force_quirks = true;
        emit_doctype();
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c)) {
        switch_to(State::BeforeDOCTYPEPublicIdentifier);
        return;
    }
    if (*c == U'"' || *c == U'\'') {
        error("missing-whitespace-after-doctype-public-keyword");
        m_current_token.public_identifier = std::string {};
        switch_to(*c == U'"' ? State::DOCTYPEPublicIdentifierDoubleQuoted : State::DOCTYPEPublicIdentifierSingleQuoted);
        return;
    }
    if (*c == U'>') {
        error("missing-doctype-public-identifier");
        m_current_token.force_quirks = true;
        switch_to(State::Data);
        emit_doctype();
        return;
    }
    error("missing-quote-before-doctype-public-identifier");
    m_current_token.force_quirks = true;
    reconsume_in(State::BogusDOCTYPE);
}

void Tokenizer::handle_before_doctype_public_identifier(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-doctype");
        m_current_token.force_quirks = true;
        emit_doctype();
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c))
        return;
    if (*c == U'"' || *c == U'\'') {
        m_current_token.public_identifier = std::string {};
        switch_to(*c == U'"' ? State::DOCTYPEPublicIdentifierDoubleQuoted : State::DOCTYPEPublicIdentifierSingleQuoted);
        return;
    }
    if (*c == U'>') {
        error("missing-doctype-public-identifier");
        m_current_token.force_quirks = true;
        switch_to(State::Data);
        emit_doctype();
        return;
    }
    error("missing-quote-before-doctype-public-identifier");
    m_current_token.force_quirks = true;
    reconsume_in(State::BogusDOCTYPE);
}

void Tokenizer::handle_doctype_public_identifier_quoted(std::optional<char32_t> c, char32_t quote)
{
    if (!c) {
        error("eof-in-doctype");
        m_current_token.force_quirks = true;
        emit_doctype();
        emit_eof();
        return;
    }
    if (*c == quote) {
        switch_to(State::AfterDOCTYPEPublicIdentifier);
        return;
    }
    if (*c == U'\0') {
        error("unexpected-null-character");
        append_utf8(*m_current_token.public_identifier, replacement_character);
        return;
    }
    if (*c == U'>') {
        error("abrupt-doctype-public-identifier");
        m_current_token.force_quirks = true;
        switch_to(State::Data);
        emit_doctype();
        return;
    }
    append_utf8(*m_current_token.public_identifier, *c);
}

void Tokenizer::handle_after_doctype_public_identifier(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-doctype");
        m_current_token.force_quirks = true;
        emit_doctype();
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c)) {
        switch_to(State::BetweenDOCTYPEPublicAndSystemIdentifiers);
        return;
    }
    if (*c == U'>') {
        switch_to(State::Data);
        emit_doctype();
        return;
    }
    if (*c == U'"' || *c == U'\'') {
        error("missing-whitespace-between-doctype-public-and-system-identifiers");
        m_current_token.system_identifier = std::string {};
        switch_to(*c == U'"' ? State::DOCTYPESystemIdentifierDoubleQuoted : State::DOCTYPESystemIdentifierSingleQuoted);
        return;
    }
    error("missing-quote-before-doctype-system-identifier");
    m_current_token.force_quirks = true;
    reconsume_in(State::BogusDOCTYPE);
}

void Tokenizer::handle_between_doctype_public_and_system(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-doctype");
        m_current_token.force_quirks = true;
        emit_doctype();
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c))
        return;
    if (*c == U'>') {
        switch_to(State::Data);
        emit_doctype();
        return;
    }
    if (*c == U'"' || *c == U'\'') {
        m_current_token.system_identifier = std::string {};
        switch_to(*c == U'"' ? State::DOCTYPESystemIdentifierDoubleQuoted : State::DOCTYPESystemIdentifierSingleQuoted);
        return;
    }
    error("missing-quote-before-doctype-system-identifier");
    m_current_token.force_quirks = true;
    reconsume_in(State::BogusDOCTYPE);
}

void Tokenizer::handle_after_doctype_system_keyword(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-doctype");
        m_current_token.force_quirks = true;
        emit_doctype();
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c)) {
        switch_to(State::BeforeDOCTYPESystemIdentifier);
        return;
    }
    if (*c == U'"' || *c == U'\'') {
        error("missing-whitespace-after-doctype-system-keyword");
        m_current_token.system_identifier = std::string {};
        switch_to(*c == U'"' ? State::DOCTYPESystemIdentifierDoubleQuoted : State::DOCTYPESystemIdentifierSingleQuoted);
        return;
    }
    if (*c == U'>') {
        error("missing-doctype-system-identifier");
        m_current_token.force_quirks = true;
        switch_to(State::Data);
        emit_doctype();
        return;
    }
    error("missing-quote-before-doctype-system-identifier");
    m_current_token.force_quirks = true;
    reconsume_in(State::BogusDOCTYPE);
}

void Tokenizer::handle_before_doctype_system_identifier(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-doctype");
        m_current_token.force_quirks = true;
        emit_doctype();
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c))
        return;
    if (*c == U'"' || *c == U'\'') {
        m_current_token.system_identifier = std::string {};
        switch_to(*c == U'"' ? State::DOCTYPESystemIdentifierDoubleQuoted : State::DOCTYPESystemIdentifierSingleQuoted);
        return;
    }
    if (*c == U'>') {
        error("missing-doctype-system-identifier");
        m_current_token.force_quirks = true;
        switch_to(State::Data);
        emit_doctype();
        return;
    }
    error("missing-quote-before-doctype-system-identifier");
    m_current_token.force_quirks = true;
    reconsume_in(State::BogusDOCTYPE);
}

void Tokenizer::handle_doctype_system_identifier_quoted(std::optional<char32_t> c, char32_t quote)
{
    if (!c) {
        error("eof-in-doctype");
        m_current_token.force_quirks = true;
        emit_doctype();
        emit_eof();
        return;
    }
    if (*c == quote) {
        switch_to(State::AfterDOCTYPESystemIdentifier);
        return;
    }
    if (*c == U'\0') {
        error("unexpected-null-character");
        append_utf8(*m_current_token.system_identifier, replacement_character);
        return;
    }
    if (*c == U'>') {
        error("abrupt-doctype-system-identifier");
        m_current_token.force_quirks = true;
        switch_to(State::Data);
        emit_doctype();
        return;
    }
    append_utf8(*m_current_token.system_identifier, *c);
}

void Tokenizer::handle_after_doctype_system_identifier(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-doctype");
        m_current_token.force_quirks = true;
        emit_doctype();
        emit_eof();
        return;
    }
    if (is_tokenizer_whitespace(*c))
        return;
    if (*c == U'>') {
        switch_to(State::Data);
        emit_doctype();
        return;
    }
    error("unexpected-character-after-doctype-system-identifier");
    reconsume_in(State::BogusDOCTYPE); // note: force-quirks NOT set here
}

void Tokenizer::handle_bogus_doctype(std::optional<char32_t> c)
{
    if (!c) {
        emit_doctype();
        emit_eof();
        return;
    }
    if (*c == U'>') {
        switch_to(State::Data);
        emit_doctype();
        return;
    }
    if (*c == U'\0')
        error("unexpected-null-character");
    // Everything else is ignored.
}

// --- CDATA -------------------------------------------------------------------

void Tokenizer::handle_cdata_section(std::optional<char32_t> c)
{
    if (!c) {
        error("eof-in-cdata");
        emit_eof();
        return;
    }
    if (*c == U']') {
        switch_to(State::CDATASectionBracket);
        return;
    }
    emit_character(*c);
}

void Tokenizer::handle_cdata_section_bracket(std::optional<char32_t> c)
{
    if (c && *c == U']') {
        switch_to(State::CDATASectionEnd);
        return;
    }
    emit_character(U']');
    reconsume_in(State::CDATASection);
}

void Tokenizer::handle_cdata_section_end(std::optional<char32_t> c)
{
    if (c && *c == U']') {
        emit_character(U']');
        return;
    }
    if (c && *c == U'>') {
        switch_to(State::Data);
        return;
    }
    emit_character(U']');
    emit_character(U']');
    reconsume_in(State::CDATASection);
}

// --- Character references ----------------------------------------------------

void Tokenizer::handle_character_reference(std::optional<char32_t> c)
{
    if (c && is_ascii_alphanumeric(*c)) {
        reconsume_in(State::NamedCharacterReference);
        return;
    }
    if (c && *c == U'#') {
        m_temp_buffer.push_back(*c);
        m_character_reference_code = 0;
        switch_to(State::NumericCharacterReference);
        return;
    }
    flush_code_points_consumed_as_character_reference();
    reconsume_in(m_return_state);
}

void Tokenizer::handle_named_character_reference()
{
    // The current character (already consumed, flagged for reconsumption) is
    // the first character of the candidate name.
    std::size_t const start = m_input.position() - (m_reconsume ? 1u : 0u);
    m_reconsume = false;
    m_input.seek(start);

    std::u32string_view const remaining = m_input.remaining();
    std::size_t window = std::min(longest_entity_name, remaining.size());

    std::string candidate;
    candidate.reserve(window);
    for (std::size_t i = 0; i < window; ++i) {
        char32_t const ch = remaining[i];
        if (ch > 0x7F)
            break;
        candidate.push_back(static_cast<char>(ch));
    }

    NamedEntity const* match = nullptr;
    std::size_t match_length = 0;
    for (std::size_t length = candidate.size(); length >= 1; --length) {
        if (NamedEntity const* entity = find_entity_exact(std::string_view(candidate).substr(0, length))) {
            match = entity;
            match_length = length;
            break;
        }
    }

    if (!match) {
        flush_code_points_consumed_as_character_reference(); // just the "&"
        switch_to(State::AmbiguousAmpersand);
        return;
    }

    m_input.advance(match_length);
    for (char const ch : match->name.substr(0, match_length))
        m_temp_buffer.push_back(static_cast<char32_t>(static_cast<unsigned char>(ch)));

    bool const has_semicolon = match->name.back() == ';';
    if (character_reference_in_attribute() && !has_semicolon && m_input.has()
        && (m_input.peek() == U'=' || is_ascii_alphanumeric(m_input.peek()))) {
        // Historical quirk: leave the text alone inside attribute values.
        flush_code_points_consumed_as_character_reference();
        switch_to(m_return_state);
        return;
    }

    if (!has_semicolon)
        error("missing-semicolon-after-character-reference");

    m_temp_buffer.clear();
    m_temp_buffer.push_back(match->first);
    if (match->second != 0)
        m_temp_buffer.push_back(match->second);
    flush_code_points_consumed_as_character_reference();
    switch_to(m_return_state);
}

void Tokenizer::handle_ambiguous_ampersand(std::optional<char32_t> c)
{
    if (c && is_ascii_alphanumeric(*c)) {
        if (character_reference_in_attribute())
            append_to_attribute_value(*c);
        else
            emit_character(*c);
        return;
    }
    if (c && *c == U';')
        error("unknown-named-character-reference");
    reconsume_in(m_return_state);
}

void Tokenizer::handle_numeric_character_reference(std::optional<char32_t> c)
{
    if (c && (*c == U'x' || *c == U'X')) {
        m_temp_buffer.push_back(*c);
        switch_to(State::HexadecimalCharacterReferenceStart);
        return;
    }
    reconsume_in(State::DecimalCharacterReferenceStart);
}

void Tokenizer::handle_hexadecimal_start(std::optional<char32_t> c)
{
    if (c && is_ascii_hex_digit(*c)) {
        reconsume_in(State::HexadecimalCharacterReference);
        return;
    }
    error("absence-of-digits-in-numeric-character-reference");
    flush_code_points_consumed_as_character_reference();
    reconsume_in(m_return_state);
}

void Tokenizer::handle_decimal_start(std::optional<char32_t> c)
{
    if (c && is_ascii_digit(*c)) {
        reconsume_in(State::DecimalCharacterReference);
        return;
    }
    error("absence-of-digits-in-numeric-character-reference");
    flush_code_points_consumed_as_character_reference();
    reconsume_in(m_return_state);
}

void Tokenizer::handle_hexadecimal(std::optional<char32_t> c)
{
    if (c && is_ascii_hex_digit(*c)) {
        if (m_character_reference_code < 0x110000)
            m_character_reference_code = m_character_reference_code * 16 + hex_digit_value(*c);
        return;
    }
    if (c && *c == U';') {
        switch_to(State::NumericCharacterReferenceEnd);
        return;
    }
    error("missing-semicolon-after-character-reference");
    reconsume_in(State::NumericCharacterReferenceEnd);
}

void Tokenizer::handle_decimal(std::optional<char32_t> c)
{
    if (c && is_ascii_digit(*c)) {
        if (m_character_reference_code < 0x110000)
            m_character_reference_code = m_character_reference_code * 10 + static_cast<char32_t>(*c - U'0');
        return;
    }
    if (c && *c == U';') {
        switch_to(State::NumericCharacterReferenceEnd);
        return;
    }
    error("missing-semicolon-after-character-reference");
    reconsume_in(State::NumericCharacterReferenceEnd);
}

void Tokenizer::handle_numeric_character_reference_end()
{
    char32_t code = m_character_reference_code;

    if (code == 0) {
        error("null-character-reference");
        code = replacement_character;
    } else if (code > 0x10FFFF) {
        error("character-reference-outside-unicode-range");
        code = replacement_character;
    } else if (is_surrogate(code)) {
        error("surrogate-character-reference");
        code = replacement_character;
    } else if (is_noncharacter(code)) {
        error("noncharacter-character-reference");
    } else if (code == 0x0D || (is_control(code) && !is_ascii_whitespace(code))) {
        error("control-character-reference");
        for (C1Remap const& entry : c1_remap) {
            if (entry.from == code) {
                code = entry.to;
                break;
            }
        }
    }

    m_temp_buffer.clear();
    m_temp_buffer.push_back(code);
    flush_code_points_consumed_as_character_reference();
    switch_to(m_return_state); // a pending reconsume, if any, carries over
}

}
