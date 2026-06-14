#include "lexer.hpp"

#include <cctype>
#include <stdexcept>
#include <unordered_map>

static const std::unordered_map<std::string, TokenType> KEYWORDS = {
    {"let", TokenType::LET},
    {"define", TokenType::DEFINE},
    {"when", TokenType::WHEN},
    {"otherwise", TokenType::ELSE},
    {"true", TokenType::TRUE},
    {"false", TokenType::FALSE},
    {"nothing", TokenType::NOTHING},
    {"and", TokenType::AND},
    {"or", TokenType::OR},
    {"not", TokenType::NOT},
    {"is", TokenType::IS},
    {"each", TokenType::EACH},
    {"in", TokenType::IN},
    {"stop", TokenType::STOP},
    {"skip", TokenType::SKIP},
    {"shape", TokenType::SHAPE},
    {"include", TokenType::INCLUDE},
    {"mutable", TokenType::MUTABLE},
    {"text", TokenType::TYPE_TEXT},
    {"number", TokenType::TYPE_NUMBER},
    {"decimal", TokenType::TYPE_DECIMAL},
    {"flag", TokenType::TYPE_FLAG},
    {"for", TokenType::FOR},
};

static const std::vector<std::pair<std::vector<std::string>, TokenType>>
    MULTIWORD = {
        {{"as", "long", "as"}, TokenType::WHILE},
        {{"give", "back"}, TokenType::RETURN},
};

static std::vector<Token> resolve_multiword(std::vector<Token> tokens) {
    std::vector<Token> out;
    int i = 0;
    while (i < (int)tokens.size()) {
        bool matched = false;
        for (auto& [words, type] : MULTIWORD) {
            if (i + (int)words.size() > (int)tokens.size()) continue;
            bool ok = true;
            for (int j = 0; j < (int)words.size(); j++) {
                if (tokens[i + j].type != TokenType::IDENTIFIER ||
                    tokens[i + j].lexeme != words[j]) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                out.emplace_back(type, tokens[i].lexeme, tokens[i].line,
                                 tokens[i].col);
                i += words.size();
                matched = true;
                break;
            }
        }
        if (!matched) out.push_back(tokens[i++]);
    }
    return out;
}

static std::vector<Token> insert_indent_tokens(std::vector<Token> tokens) {
    std::vector<Token> out;
    std::vector<int> indent_stack = {0};

    size_t i = 0;
    while (i < tokens.size()) {
        Token& tok = tokens[i];

        if (tok.type == TokenType::NEWLINE) {
            out.push_back(tok);
            i++;

            size_t j = i;
            while (j < tokens.size() && tokens[j].type == TokenType::NEWLINE)
                j++;

            int new_indent = 0;
            if (j < tokens.size() && tokens[j].type != TokenType::EOF_TOKEN)
                new_indent = tokens[j].col;

            int cur_indent = indent_stack.back();

            if (new_indent > cur_indent) {
                indent_stack.push_back(new_indent);
                out.emplace_back(TokenType::INDENT, "", tok.line, new_indent);
            } else if (new_indent < cur_indent) {
                while (indent_stack.back() > new_indent) {
                    indent_stack.pop_back();
                    out.emplace_back(TokenType::DEDENT, "", tok.line,
                                     new_indent);
                }
                if (indent_stack.back() != new_indent)
                    throw std::runtime_error(
                        "Inconsistent indentation at line " +
                        std::to_string(tok.line));
            }
        } else {
            out.push_back(tok);
            i++;
        }
    }

    while (indent_stack.size() > 1) {
        out.emplace_back(TokenType::DEDENT, "",
                         tokens.empty() ? 0 : tokens.back().line, 0);
        indent_stack.pop_back();
    }

    return out;
}

Lexer::Lexer(std::string src) : source(std::move(src)) {}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (!at_end()) {
        start = current;
        start_col = col;
        scan_token(tokens);
    }
    tokens.emplace_back(TokenType::EOF_TOKEN, "", line, col);
    auto result = resolve_multiword(std::move(tokens));
    return insert_indent_tokens(std::move(result));
}

bool Lexer::at_end() { return current >= (int)source.size(); }

char Lexer::advance() {
    col++;
    return source[current++];
}

char Lexer::peek() { return at_end() ? '\0' : source[current]; }
char Lexer::peek_next() {
    return (current + 1 >= (int)source.size()) ? '\0' : source[current + 1];
}

bool Lexer::match(char expected) {
    if (at_end() || source[current] != expected) return false;
    col++;
    current++;
    return true;
}

void Lexer::add(std::vector<Token>& tokens, TokenType type) {
    tokens.emplace_back(type, source.substr(start, current - start), line,
                        start_col);
}

void Lexer::scan_token(std::vector<Token>& tokens) {
    char c = advance();
    switch (c) {
        case '(':
            add(tokens, TokenType::LPAREN);
            break;
        case ')':
            add(tokens, TokenType::RPAREN);
            break;
        case '{':
            add(tokens, TokenType::LBRACE);
            break;
        case '}':
            add(tokens, TokenType::RBRACE);
            break;
        case ',':
            add(tokens, TokenType::COMMA);
            break;
        case '+':
            add(tokens, TokenType::PLUS);
            break;
        case '-':
            add(tokens, TokenType::MINUS);
            break;
        case '*':
            add(tokens, TokenType::STAR);
            break;
        case ':':
            add(tokens, TokenType::COLON);
            break;
        case '/':
            if (match('/')) {
                while (peek() != '\n' && !at_end()) advance();
            } else {
                add(tokens, TokenType::SLASH);
            }
            break;
        case '=':
            add(tokens, match('=') ? TokenType::EQ : TokenType::ASSIGN);
            break;
        case '!':
            add(tokens, match('=') ? TokenType::NEQ : TokenType::ILLEGAL);
            break;
        case '<':
            add(tokens, TokenType::LT);
            break;
        case '>':
            add(tokens, TokenType::GT);
            break;
        case ' ':
        case '\r':
        case '\t':
            break;
        case '\n':
            add(tokens, TokenType::NEWLINE);
            line++;
            col = 0;
            break;
        case '"':
            scan_string(tokens);
            break;
        case '[':
            add(tokens, TokenType::LBRACKET);
            break;
        case ']':
            add(tokens, TokenType::RBRACKET);
            break;
        default:
            if (std::isdigit(c))
                scan_number(tokens);
            else if (std::isalpha(c) || c == '_')
                scan_identifier(tokens);
            else
                add(tokens, TokenType::ILLEGAL);
    }
}

void Lexer::scan_string(std::vector<Token>& tokens) {
    while (peek() != '"' && !at_end()) {
        if (peek() == '\n') {
            line++;
            col = 0;
        }
        advance();
    }
    if (at_end())
        throw std::runtime_error("Unterminated string at line " +
                                 std::to_string(line));
    advance();
    add(tokens, TokenType::STRING);
}

void Lexer::scan_number(std::vector<Token>& tokens) {
    while (std::isdigit(peek())) advance();
    if (peek() == '.' && std::isdigit(peek_next())) {
        advance();
        while (std::isdigit(peek())) advance();
    }
    add(tokens, TokenType::NUMBER);
}

void Lexer::scan_identifier(std::vector<Token>& tokens) {
    while (std::isalnum(peek()) || peek() == '_') advance();
    std::string word = source.substr(start, current - start);
    auto it = KEYWORDS.find(word);
    TokenType type =
        (it != KEYWORDS.end()) ? it->second : TokenType::IDENTIFIER;
    add(tokens, type);
}
