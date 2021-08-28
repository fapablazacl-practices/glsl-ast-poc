#include "CLI/CLI.hpp"

#include "fmt/format.h"
#include "fmt/ostream.h"

#include "nlohmann/json.hpp"

#include "mongoose.h"

#include "ResourceLimits.h"
#include "ShaderLang.h"
#include "glslang/MachineIndependent/localintermediate.h"
#include "glslang/Include/intermediate.h"

#include <cstdint>
#include <experimental/filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "messagebuffer.hpp"
#include "workspace.hpp"
#include "utils.hpp"

using json = nlohmann::json;
namespace fs = std::experimental::filesystem;

struct AppState {
    Workspace workspace;
    bool verbose;
    bool use_logfile;
    std::ofstream logfile_stream;
};

std::string make_response(const json& response)
{
    json content = response;
    content["jsonrpc"] = "2.0";

    std::string header;
    header.append("Content-Length: " + std::to_string(content.dump(4).size()) + "\r\n");
    header.append("Content-Type: application/vscode-jsonrpc;charset=utf-8\r\n");
    header.append("\r\n");
    return header + content.dump(4);
}

EShLanguage find_language(const std::string& name)
{
    auto ext = fs::path(name).extension();
    if (ext == ".vert")
        return EShLangVertex;
    else if (ext == ".tesc")
        return EShLangTessControl;
    else if (ext == ".tese")
        return EShLangTessEvaluation;
    else if (ext == ".geom")
        return EShLangGeometry;
    else if (ext == ".frag")
        return EShLangFragment;
    else if (ext == ".comp")
        return EShLangCompute;
    throw std::invalid_argument("Unknown file extension!");
}

json get_diagnostics(std::string uri, std::string content,
        AppState& appstate)
{
    FILE fp_old = *stdout;
    *stdout = *fopen("/dev/null","w");
    auto document = uri;
    auto shader_cstring = content.c_str();
    auto lang = find_language(document);
    glslang::InitializeProcess();
    glslang::TShader shader(lang);
    shader.setStrings(&shader_cstring, 1);
    TBuiltInResource Resources = glslang::DefaultTBuiltInResource;
    EShMessages messages = EShMsgCascadingErrors;
    shader.parse(&Resources, 110, false, messages);
    std::string debug_log = shader.getInfoLog();

    // ACA SE PUEDE IMPLEMENTAR LA AYUDA CONTEXTUAL!!
    // shader.getIntermediate()->getTreeRoot()

    glslang::FinalizeProcess();
    *stdout = fp_old;

    if (appstate.use_logfile && appstate.verbose) {
        fmt::print(appstate.logfile_stream, "Diagnostics raw output: {}\n" , debug_log);
    }

    std::regex re("(.*): 0:(\\d*): (.*)");
    std::smatch matches;
    auto error_lines = split_string(debug_log, "\n");
    auto content_lines = split_string(content, "\n");

    json diagnostics;
    for (auto error_line : error_lines) {
        std::regex_search(error_line, matches, re);
        if (matches.size() == 4) {
            json diagnostic;
            std::string severity = matches[1];
            int severity_no = -1;
            if (severity == "ERROR") {
                severity_no = 1;
            } else if (severity == "WARNING") {
                severity_no = 2;
            }
            if (severity_no == -1) {
                if (appstate.use_logfile) {
                    fmt::print(appstate.logfile_stream, "Error: Unknown severity '{}'\n", severity);
                }
            }
            std::string message = trim(matches[3], " ");

            // -1 because lines are 0-indexed as per LSP specification.
            int line_no = std::stoi(matches[2]) - 1;
            std::string source_line = content_lines[line_no];

            int start_char = -1;
            int end_char = -1;

            // If this is an undeclared identifier, we can find the exact
            // position of the broken identifier.
            std::smatch message_matches;
            std::regex re("'(.*)' : (.*)");
            std::regex_search(message, message_matches, re);
            if (message_matches.size() == 3) {
                std::string identifier = message_matches[1];
                int identifier_length = message_matches[1].length();
                auto source_pos = source_line.find(identifier);
                start_char = source_pos;
                end_char = source_pos + identifier_length - 1;
            } else {
                // If we can't find a precise position, we'll just use the whole line.
                start_char = 0;
                end_char = source_line.length();
            }

            json range{
                {"start", {
                    { "line", line_no },
                    { "character", start_char },
                }},
                { "end", {
                    { "line", line_no },
                    { "character", end_char },
                }},
            };
            diagnostic["range"] = range;
            diagnostic["severity"] = severity_no;
            diagnostic["source"] = "glslang";
            diagnostic["message"] = message;
            diagnostics.push_back(diagnostic);
        }
    }
    if (appstate.use_logfile && appstate.verbose) {
        fmt::print(appstate.logfile_stream, "Sending diagnostics: {}\n" , diagnostics);
    }
    appstate.logfile_stream.flush();
    return diagnostics;
}

std::optional<std::string> handle_message(const MessageBuffer& message_buffer, AppState& appstate)
{
    json body = message_buffer.body();

    if (body["method"] == "initialized") {
        return std::nullopt;
    }

    if (body["method"] == "initialize") {
        appstate.workspace.set_initialized(true);

        json text_document_sync{
            { "openClose", true },
            { "change", 1 }, // Full sync
            { "willSave", false },
            { "willSaveWaitUntil", false },
            { "save", { { "includeText", false } } },
        };

        json completion_provider{
            { "resolveProvider", false },
            { "triggerCharacters", {} },
        };
        json signature_help_provider{
            { "triggerCharacters", "" }
        };
        json code_lens_provider{
            { "resolveProvider", false }
        };
        json document_on_type_formatting_provider{
            { "firstTriggerCharacter", "" },
            { "moreTriggerCharacter", "" },
        };
        json document_link_provider{
            { "resolveProvider", false }
        };
        json execute_command_provider{
            { "commands", {} }
        };
        json result{
            {
                "capabilities",
                {
                { "textDocumentSync", text_document_sync },
                { "hoverProvider", false },
                { "completionProvider", completion_provider },
                { "signatureHelpProvider", signature_help_provider },
                { "definitionProvider", false },
                { "referencesProvider", false },
                { "documentHighlightProvider", false },
                { "documentSymbolProvider", false },
                { "workspaceSymbolProvider", false },
                { "codeActionProvider", false },
                { "codeLensProvider", code_lens_provider },
                { "documentFormattingProvider", false },
                { "documentRangeFormattingProvider", false },
                { "documentOnTypeFormattingProvider", document_on_type_formatting_provider },
                { "renameProvider", false },
                { "documentLinkProvider", document_link_provider },
                { "executeCommandProvider", execute_command_provider },
                { "experimental", {} }, }
            }
        };

        json result_body{
            { "id", body["id"] },
            { "result", result }
        };
        return make_response(result_body);
    } else if (body["method"] == "textDocument/didOpen") {
        auto uri = body["params"]["textDocument"]["uri"];
        auto text = body["params"]["textDocument"]["text"];
        appstate.workspace.add_document(uri, text);

        json diagnostics = get_diagnostics(uri, text, appstate);
        if (diagnostics.empty()) {
            diagnostics = json::array();
        }
        json result_body{
            { "method", "textDocument/publishDiagnostics" },
            { "params", {
                            { "uri", uri },
                            { "diagnostics", diagnostics },
                        } }
        };
        return make_response(result_body);
    } else if (body["method"] == "textDocument/didChange") {
        auto uri = body["params"]["textDocument"]["uri"];
        auto change = body["params"]["contentChanges"][0]["text"];
        appstate.workspace.change_document(uri, change);

        std::string document = appstate.workspace.documents()[uri];
        json diagnostics = get_diagnostics(uri, document, appstate);
        if (diagnostics.empty()) {
            diagnostics = json::array();
        }
        json result_body{
            { "method", "textDocument/publishDiagnostics" },
            { "params", {
                            { "uri", uri },
                            { "diagnostics", diagnostics },
                        } }
        };
        return make_response(result_body);
    }

    // If the workspace has not yet been initialized but the client sends a
    // message that doesn't have method "initialize" then we'll return an error
    // as per LSP spec.
    if (body["method"] != "initialize" && !appstate.workspace.is_initialized()) {
        json error{
            { "code", -32002 },
            { "message", "Server not yet initialized." },
        };
        json result_body{
            { "error", error }
        };
        return make_response(result_body);
    }

    // If we don't know the method requested, we end up here.
    if (body.count("method") == 1) {
        json error{
            { "code", -32601 },
            { "message", fmt::format("Method '{}' not supported.", body["method"].get<std::string>()) },
        };
        json result_body{
            { "error", error }
        };
        return make_response(result_body);
    }

    // If we couldn't parse anything we end up here.
    json error{
        { "code", -32700 },
        { "message", "Couldn't parse message." },
    };
    json result_body{
        { "error", error }
    };
    return make_response(result_body);
}

void ev_handler(struct mg_connection* c, int ev, void* p) {
    AppState& appstate = *static_cast<AppState*>(c->mgr->user_data);

    if (ev == MG_EV_HTTP_REQUEST) {
        struct http_message* hm = (struct http_message*)p;

        std::string content = hm->message.p;

        MessageBuffer message_buffer;
        message_buffer.handle_string(content);

        if (message_buffer.message_completed()) {
            json body = message_buffer.body();
            if (appstate.use_logfile) {
                fmt::print(appstate.logfile_stream, ">>> Received message of type '{}'\n", body["method"].get<std::string>());
                if (appstate.verbose) {
                    fmt::print(appstate.logfile_stream, "Headers:\n");
                    for (auto elem : message_buffer.headers()) {
                        auto pretty_header = fmt::format("{}: {}\n", elem.first, elem.second);
                        appstate.logfile_stream << pretty_header;
                    }
                    fmt::print(appstate.logfile_stream, "Body: \n{}\n\n", body.dump(4));
                    fmt::print(appstate.logfile_stream, "Raw: \n{}\n\n", message_buffer.raw());
                }
            }

            auto message = handle_message(message_buffer, appstate);
            if (message.has_value()) {
                std::string response = message.value();
                mg_send_head(c, 200, response.length(), "Content-Type: text/plain");
                mg_printf(c, "%.*s", static_cast<int>(response.length()), response.c_str());
                if (appstate.use_logfile && appstate.verbose) {
                    fmt::print(appstate.logfile_stream, "<<< Sending message: \n{}\n\n", message.value());
                }
            }
            appstate.logfile_stream.flush();
            message_buffer.clear();
        }
    }
}

const std::string document = "shader.vert";
const std::string content = R"(
#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) out vec3 fragColor;

vec2 positions[3] = vec2[](
	vec2(0.0, -0.5),
	vec2(0.0, 0.5),
	vec2(-0.0, 0.5)
);

vec3 colors[3] = vec3[](
	vec3(1.0, 0.0, 0.0), 
	vec3(0.0, 1.0, 0.0), 
	vec3(0.0, 0.0, 1.0)
);

void main() {
    vec4 testVector = {0.0f, 0.0f, 1.0f, 1.0f};

	gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0) + testVector;
	fragColor = colors[gl_VertexIndex];
}
)";


class FindSymbolTraverser : public glslang::TIntermTraverser {
public:
    FindSymbolTraverser(const int line, const int column) {
        this->line = line;
        this->column = column;
    }

    glslang::TIntermSymbol* getSymbol() const {
        return symbol;
    }

private:
    int line, column;
    glslang::TIntermSymbol* symbol = nullptr;

private:
    void visitSymbol(glslang::TIntermSymbol* interm) override {
        std::cout << "visitSymbol " << interm->getLoc().line << ":" << interm->getLoc().column << ", " << interm->getName() << std::endl;

        const auto loc = interm->getLoc();
        const auto name = interm->getName();

        if (line == loc.line) {
            const auto start = loc.column;
            const auto end = loc.column + name.size();

            if (column >= start && column <= end) {
                symbol = interm;
            }
        }
    }

    virtual void visitConstantUnion(glslang::TIntermConstantUnion* interm) {
        std::cout << "visitConstantUnion " << interm->getLoc().line << ":" << interm->getLoc().column << std::endl;
    }

    virtual bool visitBinary(glslang::TVisit visit, glslang::TIntermBinary* interm)       { 
        std::cout << "visitBinary " << interm->getLoc().line << ":" << interm->getLoc().column << std::endl;
        return true; 
    }

    virtual bool visitUnary(glslang::TVisit, glslang::TIntermUnary* interm)         { 
        std::cout << "visitUnary " << interm->getLoc().line << ":" << interm->getLoc().column << std::endl;
        return true; 
    }

    virtual bool visitSelection(glslang::TVisit, glslang::TIntermSelection* interm) { 
        std::cout << "visitSelection " << interm->getLoc().line << ":" << interm->getLoc().column << std::endl;
        return true; 
    }

    virtual bool visitAggregate(glslang::TVisit, glslang::TIntermAggregate* interm) { 
        std::cout << "visitAggregate " << interm->getLoc().line << ":" << interm->getLoc().column << std::endl;
        return true; 
    }

    virtual bool visitLoop(glslang::TVisit, glslang::TIntermLoop* interm)           { 
        std::cout << "visitLoop " << interm->getLoc().line << ":" << interm->getLoc().column << std::endl;
        return true; 
    }

    virtual bool visitBranch(glslang::TVisit, glslang::TIntermBranch* interm) { 
        std::cout << "visitBranch " << interm->getLoc().line << ":" << interm->getLoc().column << std::endl;
        return true; 
    }

    virtual bool visitSwitch(glslang::TVisit, glslang::TIntermSwitch* interm) {
        std::cout << "visitSwitch " << interm->getLoc().line << ":" << interm->getLoc().column << std::endl;
        return true; 
    }
};

int main(int argc, char* argv[]) {
    auto shader_cstring = content.c_str();
    auto lang = find_language(document);
    glslang::InitializeProcess();
    glslang::TShader shader(lang);
    shader.setStrings(&shader_cstring, 1);
    TBuiltInResource Resources = glslang::DefaultTBuiltInResource;
    EShMessages messages = EShMsgCascadingErrors;
    shader.parse(&Resources, 110, false, messages);
    std::string debug_log = shader.getInfoLog();

    // ACA SE PUEDE IMPLEMENTAR LA AYUDA CONTEXTUAL!!
    const auto intermediate = shader.getIntermediate();
    const auto root = intermediate->getTreeRoot();

    FindSymbolTraverser traverser{19, 13};

    root->traverse(&traverser);

    const auto symbol = traverser.getSymbol();

    if (symbol) {
        std::cout 
            << symbol->getLoc().line << ":" << symbol->getLoc().column << " -> " 
            << symbol->getName() << ":" << symbol->getWritableType().getBasicTypeString() 
            << std::endl;
    } else {
        std::cout << "no symbol located!";
    }

    glslang::FinalizeProcess();

    return 0;
}
