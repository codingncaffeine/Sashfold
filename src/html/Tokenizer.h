#pragma once

#include "html/InputStream.h"
#include "html/Token.h"

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::html {

// The tokenization stage of WHATWG HTML §13.2.5, state for state. Errors are
// collected, never thrown: hostile input must degrade, not stop.
class Tokenizer {
public:
    enum class State {
        Data,
        RCDATA,
        RAWTEXT,
        ScriptData,
        PLAINTEXT,
        TagOpen,
        EndTagOpen,
        TagName,
        RCDATALessThanSign,
        RCDATAEndTagOpen,
        RCDATAEndTagName,
        RAWTEXTLessThanSign,
        RAWTEXTEndTagOpen,
        RAWTEXTEndTagName,
        ScriptDataLessThanSign,
        ScriptDataEndTagOpen,
        ScriptDataEndTagName,
        ScriptDataEscapeStart,
        ScriptDataEscapeStartDash,
        ScriptDataEscaped,
        ScriptDataEscapedDash,
        ScriptDataEscapedDashDash,
        ScriptDataEscapedLessThanSign,
        ScriptDataEscapedEndTagOpen,
        ScriptDataEscapedEndTagName,
        ScriptDataDoubleEscapeStart,
        ScriptDataDoubleEscaped,
        ScriptDataDoubleEscapedDash,
        ScriptDataDoubleEscapedDashDash,
        ScriptDataDoubleEscapedLessThanSign,
        ScriptDataDoubleEscapeEnd,
        BeforeAttributeName,
        AttributeName,
        AfterAttributeName,
        BeforeAttributeValue,
        AttributeValueDoubleQuoted,
        AttributeValueSingleQuoted,
        AttributeValueUnquoted,
        AfterAttributeValueQuoted,
        SelfClosingStartTag,
        BogusComment,
        MarkupDeclarationOpen,
        CommentStart,
        CommentStartDash,
        Comment,
        CommentLessThanSign,
        CommentLessThanSignBang,
        CommentLessThanSignBangDash,
        CommentLessThanSignBangDashDash,
        CommentEndDash,
        CommentEnd,
        CommentEndBang,
        DOCTYPE,
        BeforeDOCTYPEName,
        DOCTYPEName,
        AfterDOCTYPEName,
        AfterDOCTYPEPublicKeyword,
        BeforeDOCTYPEPublicIdentifier,
        DOCTYPEPublicIdentifierDoubleQuoted,
        DOCTYPEPublicIdentifierSingleQuoted,
        AfterDOCTYPEPublicIdentifier,
        BetweenDOCTYPEPublicAndSystemIdentifiers,
        AfterDOCTYPESystemKeyword,
        BeforeDOCTYPESystemIdentifier,
        DOCTYPESystemIdentifierDoubleQuoted,
        DOCTYPESystemIdentifierSingleQuoted,
        AfterDOCTYPESystemIdentifier,
        BogusDOCTYPE,
        CDATASection,
        CDATASectionBracket,
        CDATASectionEnd,
        CharacterReference,
        NamedCharacterReference,
        AmbiguousAmpersand,
        NumericCharacterReference,
        HexadecimalCharacterReferenceStart,
        DecimalCharacterReferenceStart,
        HexadecimalCharacterReference,
        DecimalCharacterReference,
        NumericCharacterReferenceEnd,
    };

    struct ParseError {
        std::string code; // spec error name, e.g. "unexpected-null-character"
        std::size_t position;
    };

    explicit Tokenizer(std::string_view utf8_input);
    Tokenizer(InputStream input, State initial_state, std::string last_start_tag_name = {});

    // Tokens until (and including) EndOfFile; nullopt afterwards.
    std::optional<Token> next_token();

    std::vector<ParseError> const& errors() const { return m_errors; }

    // Tree-builder hooks.
    void set_state(State state) { m_state = state; }
    // The input, for document.write: a script run by the parser inserts
    // text just before the next character to be consumed.
    InputStream& input() { return m_input; }
    void set_in_foreign_content(bool value) { m_in_foreign_content = value; }

    // Maps html5lib fixture names ("Data state", "RCDATA state", ...).
    static std::optional<State> state_from_test_name(std::string_view);

private:
    void step();
    void dispatch(std::optional<char32_t>);

    // States that operate on lookahead without consuming first.
    void handle_markup_declaration_open();
    void handle_named_character_reference();
    void handle_numeric_character_reference_end();

    void handle_data(std::optional<char32_t>);
    void handle_rcdata(std::optional<char32_t>);
    void handle_rawtext(std::optional<char32_t>);
    void handle_script_data(std::optional<char32_t>);
    void handle_plaintext(std::optional<char32_t>);
    void handle_tag_open(std::optional<char32_t>);
    void handle_end_tag_open(std::optional<char32_t>);
    void handle_tag_name(std::optional<char32_t>);
    void handle_less_than_sign(std::optional<char32_t>, State end_tag_open_state, State return_text_state, bool script_bang);
    void handle_text_end_tag_open(std::optional<char32_t>, State end_tag_name_state, State return_text_state);
    void handle_text_end_tag_name(std::optional<char32_t>, State return_text_state);
    void handle_script_data_escape_start(std::optional<char32_t>);
    void handle_script_data_escape_start_dash(std::optional<char32_t>);
    void handle_script_data_escaped(std::optional<char32_t>);
    void handle_script_data_escaped_dash(std::optional<char32_t>);
    void handle_script_data_escaped_dash_dash(std::optional<char32_t>);
    void handle_script_data_escaped_less_than_sign(std::optional<char32_t>);
    void handle_script_data_double_escape_start_or_end(std::optional<char32_t>, State on_match, State on_mismatch);
    void handle_script_data_double_escaped(std::optional<char32_t>);
    void handle_script_data_double_escaped_dash(std::optional<char32_t>);
    void handle_script_data_double_escaped_dash_dash(std::optional<char32_t>);
    void handle_script_data_double_escaped_less_than_sign(std::optional<char32_t>);
    void handle_before_attribute_name(std::optional<char32_t>);
    void handle_attribute_name(std::optional<char32_t>);
    void handle_after_attribute_name(std::optional<char32_t>);
    void handle_before_attribute_value(std::optional<char32_t>);
    void handle_attribute_value_quoted(std::optional<char32_t>, char32_t quote);
    void handle_attribute_value_unquoted(std::optional<char32_t>);
    void handle_after_attribute_value_quoted(std::optional<char32_t>);
    void handle_self_closing_start_tag(std::optional<char32_t>);
    void handle_bogus_comment(std::optional<char32_t>);
    void handle_comment_start(std::optional<char32_t>);
    void handle_comment_start_dash(std::optional<char32_t>);
    void handle_comment(std::optional<char32_t>);
    void handle_comment_less_than_sign(std::optional<char32_t>);
    void handle_comment_less_than_sign_bang(std::optional<char32_t>);
    void handle_comment_less_than_sign_bang_dash(std::optional<char32_t>);
    void handle_comment_less_than_sign_bang_dash_dash(std::optional<char32_t>);
    void handle_comment_end_dash(std::optional<char32_t>);
    void handle_comment_end(std::optional<char32_t>);
    void handle_comment_end_bang(std::optional<char32_t>);
    void handle_doctype(std::optional<char32_t>);
    void handle_before_doctype_name(std::optional<char32_t>);
    void handle_doctype_name(std::optional<char32_t>);
    void handle_after_doctype_name(std::optional<char32_t>);
    void handle_after_doctype_public_keyword(std::optional<char32_t>);
    void handle_before_doctype_public_identifier(std::optional<char32_t>);
    void handle_doctype_public_identifier_quoted(std::optional<char32_t>, char32_t quote);
    void handle_after_doctype_public_identifier(std::optional<char32_t>);
    void handle_between_doctype_public_and_system(std::optional<char32_t>);
    void handle_after_doctype_system_keyword(std::optional<char32_t>);
    void handle_before_doctype_system_identifier(std::optional<char32_t>);
    void handle_doctype_system_identifier_quoted(std::optional<char32_t>, char32_t quote);
    void handle_after_doctype_system_identifier(std::optional<char32_t>);
    void handle_bogus_doctype(std::optional<char32_t>);
    void handle_cdata_section(std::optional<char32_t>);
    void handle_cdata_section_bracket(std::optional<char32_t>);
    void handle_cdata_section_end(std::optional<char32_t>);
    void handle_character_reference(std::optional<char32_t>);
    void handle_ambiguous_ampersand(std::optional<char32_t>);
    void handle_numeric_character_reference(std::optional<char32_t>);
    void handle_hexadecimal_start(std::optional<char32_t>);
    void handle_decimal_start(std::optional<char32_t>);
    void handle_hexadecimal(std::optional<char32_t>);
    void handle_decimal(std::optional<char32_t>);

    void switch_to(State state) { m_state = state; }
    void reconsume_in(State state)
    {
        m_state = state;
        m_reconsume = true;
    }

    void error(std::string_view code);

    void emit_character(char32_t);
    void emit_temp_buffer_as_characters();
    void emit_eof();
    void emit_current_tag();
    void emit_comment();
    void emit_doctype();

    void create_start_tag();
    void create_end_tag();
    void create_comment(std::string initial_data = {});
    void create_doctype();

    void start_new_attribute();
    void commit_current_attribute();
    void finish_attribute_name();
    void append_to_attribute_value(char32_t);

    bool current_end_tag_is_appropriate() const;

    void start_character_reference(State return_state);
    bool character_reference_in_attribute() const;
    void flush_code_points_consumed_as_character_reference();

    InputStream m_input;
    State m_state = State::Data;
    State m_return_state = State::Data;

    bool m_reconsume = false;
    std::optional<char32_t> m_current;

    std::deque<Token> m_queue;
    bool m_eof_emitted = false;

    Token m_current_token;
    bool m_has_current_attribute = false;
    bool m_current_attribute_ignored = false;

    std::string m_last_start_tag_name;
    std::u32string m_temp_buffer;
    char32_t m_character_reference_code = 0;
    bool m_in_foreign_content = false;

    std::vector<ParseError> m_errors;
};

}
