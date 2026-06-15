#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "borrowchecker.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "typechecker.hpp"

using json = nlohmann::json;

// ── JSON-RPC transport ────────────────────────────────────────────────────────
// LSP uses HTTP-like framing over stdin/stdout:
//   Content-Length: 123\r\n
//   \r\n
//   {"jsonrpc":"2.0", ...}

static std::string read_message() {
    int content_length = -1;

    while (true) {
        std::string line;
        std::getline(std::cin, line);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if (line.rfind("Content-Length: ", 0) == 0)
            content_length = std::stoi(line.substr(16));
    }

    if (content_length < 0) return "";
    std::string body(content_length, '\0');
    std::cin.read(body.data(), content_length);
    return body;
}

static void send_message(const json& msg) {
    std::string body = msg.dump();
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
}

static void send_response(const json& id, const json& result) {
    send_message({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

static void send_notification(const std::string& method, const json& params) {
    send_message({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

// ── Helpers ───────────────────────────────────────────────────────────────────

// LSP URIs look like "file:///home/user/foo.sprig" — strip the scheme so the
// type checker can use it as a filesystem path for include resolution.
static std::string uri_to_path(const std::string& uri) {
    if (uri.size() >= 7 && uri.substr(0, 7) == "file://")
        return uri.substr(7);
    return uri;
}

static json make_diagnostic(int line, const std::string& message, int severity = 1) {
    int lsp_line = std::max(0, line - 1); // LSP lines are 0-indexed
    return {
        {"range", {
            {"start", {{"line", lsp_line}, {"character", 0}}},
            {"end",   {{"line", lsp_line}, {"character", 999}}}
        }},
        {"severity", severity},
        {"source",   "sprig"},
        {"message",  message}
    };
}

// ── Analysis pipeline ─────────────────────────────────────────────────────────

static json analyse(const std::string& source, const std::string& uri) {
    json diagnostics = json::array();
    std::string path = uri_to_path(uri);

    try {
        Lexer lexer(source);
        std::vector<Token> tokens;
        try {
            tokens = lexer.tokenize();
        } catch (std::exception& e) {
            diagnostics.push_back(make_diagnostic(1, e.what()));
            return diagnostics;
        }

        Parser parser(tokens);
        Program program;
        try {
            program = parser.parse();
        } catch (std::exception& e) {
            diagnostics.push_back(make_diagnostic(1, e.what()));
            return diagnostics;
        }

        TypeChecker type_checker;
        for (auto& err : type_checker.check(program, source, path))
            diagnostics.push_back(make_diagnostic(err.line, err.message));

        BorrowChecker borrow_checker;
        for (auto& err : borrow_checker.check(program, source))
            diagnostics.push_back(make_diagnostic(err.line, err.message));

    } catch (std::exception& e) {
        diagnostics.push_back(make_diagnostic(1, e.what()));
    }

    return diagnostics;
}

// ── Document store ────────────────────────────────────────────────────────────

static std::unordered_map<std::string, std::string> documents;

static void publish(const std::string& uri, const std::string& source) {
    send_notification("textDocument/publishDiagnostics", {
        {"uri",         uri},
        {"diagnostics", analyse(source, uri)}
    });
}

// ── Main loop ─────────────────────────────────────────────────────────────────

int main() {
    while (std::cin.good()) {
        std::string body = read_message();
        if (body.empty()) continue;

        json msg;
        try { msg = json::parse(body); } catch (...) { continue; }

        std::string method = msg.value("method", "");

        if (method == "initialize") {
            send_response(msg["id"], {
                {"capabilities", {
                    {"textDocumentSync", 1},        // 1 = full sync
                    {"diagnosticProvider", false}   // push-based via publishDiagnostics
                }},
                {"serverInfo", {{"name", "sprig-lsp"}, {"version", "0.1.0"}}}
            });
        }
        else if (method == "initialized") {
            // notification — no response needed
        }
        else if (method == "textDocument/didOpen") {
            auto& td           = msg["params"]["textDocument"];
            std::string uri    = td["uri"];
            std::string source = td["text"];
            documents[uri]     = source;
            publish(uri, source);
        }
        else if (method == "textDocument/didChange") {
            std::string uri   = msg["params"]["textDocument"]["uri"];
            auto& changes     = msg["params"]["contentChanges"];
            if (!changes.empty()) {
                std::string source = changes[0]["text"];
                documents[uri]     = source;
                publish(uri, source);
            }
        }
        else if (method == "textDocument/didSave") {
            std::string uri = msg["params"]["textDocument"]["uri"];
            auto it = documents.find(uri);
            if (it != documents.end()) publish(uri, it->second);
        }
        else if (method == "textDocument/didClose") {
            std::string uri = msg["params"]["textDocument"]["uri"];
            documents.erase(uri);
            send_notification("textDocument/publishDiagnostics", {
                {"uri", uri}, {"diagnostics", json::array()}
            });
        }
        else if (method == "shutdown") {
            send_response(msg["id"], nullptr);
        }
        else if (method == "exit") {
            return 0;
        }
        else if (msg.contains("id")) {
            send_response(msg["id"], nullptr);
        }
    }
    return 0;
}
