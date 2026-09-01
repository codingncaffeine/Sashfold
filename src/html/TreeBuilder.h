#pragma once

// Tree construction, WHATWG HTML §13.2.6: the insertion-mode machine that
// turns tokenizer output into a Document. Scripting is off (no JS yet), so
// <noscript> parses as content and <script> text is kept, never run.

#include "dom/Dom.h"
#include "html/Token.h"
#include "html/Tokenizer.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::html {

class TreeBuilder {
public:
    // Whole-document parsing.
    explicit TreeBuilder(dom::Document&);

    // Fragment parsing (§13.4); the context element decides tokenizer state
    // and the initial insertion mode. Parsed children land under the builder's
    // root <html> element.
    TreeBuilder(dom::Document&, dom::Element& context);

    // Pumps the tokenizer to end of input.
    void run(Tokenizer&);

    dom::Element* root_html_element() const { return m_html_element; }

    // Chooses the tokenizer start state a fragment context demands.
    static Tokenizer::State tokenizer_state_for_fragment_context(dom::Element const&);

private:
    enum class Mode {
        Initial,
        BeforeHtml,
        BeforeHead,
        InHead,
        InHeadNoscript,
        AfterHead,
        InBody,
        Text,
        InTable,
        InTableText,
        InCaption,
        InColumnGroup,
        InTableBody,
        InRow,
        InCell,
        InTemplate,
        AfterBody,
        InFrameset,
        AfterFrameset,
        AfterAfterBody,
        AfterAfterFrameset,
    };

    struct FormattingEntry {
        dom::Element* element = nullptr; // nullptr == scope marker
        Token token; // the start tag that created it, for cloning
    };

    struct InsertionLocation {
        dom::Node* parent = nullptr;
        dom::Node* before = nullptr; // nullptr == append
    };

    void process(Token&);
    bool dispatch(Token&); // returns true to reprocess
    bool process_mode(Mode, Token&);
    bool use_foreign_rules(Token const&) const;
    bool process_foreign(Token&);

    bool mode_initial(Token&);
    bool mode_before_html(Token&);
    bool mode_before_head(Token&);
    bool mode_in_head(Token&);
    bool mode_in_head_noscript(Token&);
    bool mode_after_head(Token&);
    bool mode_in_body(Token&);
    bool mode_text(Token&);
    bool mode_in_table(Token&);
    bool mode_in_table_text(Token&);
    bool mode_in_caption(Token&);
    bool mode_in_column_group(Token&);
    bool mode_in_table_body(Token&);
    bool mode_in_row(Token&);
    bool mode_in_cell(Token&);
    bool mode_in_template(Token&);
    bool mode_after_body(Token&);
    bool mode_in_frameset(Token&);
    bool mode_after_frameset(Token&);
    bool mode_after_after_body(Token&);
    bool mode_after_after_frameset(Token&);

    // Stack of open elements.
    dom::Element* current_node() const;
    dom::Element* adjusted_current_node() const;
    dom::Element* node_above(dom::Element*) const;
    void pop();
    void pop_until_html_element_popped(std::string_view name);
    void pop_until_popped(dom::Element*);
    void remove_from_stack(dom::Element*);
    bool stack_contains(dom::Element*) const;
    bool stack_has_template() const;
    void clear_stack_to_table_context();
    void clear_stack_to_table_body_context();
    void clear_stack_to_table_row_context();

    bool has_in_scope(std::string_view name) const;
    bool has_in_scope(dom::Element*) const;
    bool has_in_list_item_scope(std::string_view name) const;
    bool has_in_button_scope(std::string_view name) const;
    bool has_in_table_scope(std::string_view name) const;
    bool has_heading_in_scope() const;

    void generate_implied_end_tags(std::string_view except = {});
    void generate_implied_end_tags_thoroughly();

    // Insertion.
    InsertionLocation appropriate_place(dom::Node* override_target = nullptr) const;
    dom::Element* create_element_for_token(Token const&, std::string_view namespace_uri);
    dom::Element* insert_html_element(Token const&);
    dom::Element* insert_foreign_element(Token const&, std::string_view namespace_uri);
    void insert_character(char32_t);
    void insert_comment(Token const&, dom::Node* explicit_parent = nullptr);

    // Active formatting elements.
    void push_formatting(dom::Element*, Token const&);
    void push_formatting_marker();
    void clear_formatting_to_marker();
    void reconstruct_formatting();
    int formatting_index_after_marker(std::string_view name) const; // -1 if absent
    void adoption_agency(Token&);
    bool any_other_end_tag_in_body(Token&);

    void close_p_element();
    void close_cell();
    void reset_insertion_mode();
    void parse_generic_text(Token const&, Tokenizer::State);
    void merge_attributes_into(dom::Element&, Token const&);
    void stop_parsing(); // pops the whole stack (option pops can clone)
    void maybe_clone_option_into_selectedcontent(dom::Element& option);

    dom::Document& m_document;
    Tokenizer* m_tokenizer = nullptr;

    Mode m_mode = Mode::Initial;
    Mode m_original_mode = Mode::Initial;
    std::vector<Mode> m_template_modes;

    std::vector<dom::Element*> m_stack;
    std::vector<FormattingEntry> m_formatting;

    dom::Element* m_html_element = nullptr;
    dom::Element* m_head_element = nullptr;
    dom::Element* m_form_element = nullptr;
    dom::Element* m_context = nullptr; // fragment parsing

    bool m_frameset_ok = true;
    bool m_foster_parenting = false;
    bool m_ignore_next_linefeed = false;
    bool m_done = false;

    std::u32string m_pending_table_characters;
};

// Convenience: parse a complete document / a fragment.
std::unique_ptr<dom::Document> parse_document(std::string_view utf8);
std::unique_ptr<dom::Document> parse_document(std::u32string code_points);

// Sniffs the encoding first (BOM, meta prescan, windows-1252 fallback).
std::unique_ptr<dom::Document> parse_document_bytes(std::string_view bytes);

struct FragmentParseResult {
    std::unique_ptr<dom::Document> document;
    dom::Element* root = nullptr; // fragment children hang under this
};
FragmentParseResult parse_fragment(std::u32string code_points, std::string_view context_namespace,
    std::string_view context_local_name);

}
