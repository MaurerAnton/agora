#include "agora/core/embedding.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/openai_request.hpp"

namespace agora::core::embedding {
namespace {

namespace json = agora::core::json;
namespace openai = agora::core::openai;

const json::Value* member(const json::Value& object, std::string_view name) {
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;  // last duplicate wins (LinkedHashMap)
    }
    return found;
}

bool is_null(const json::Value* value) {
    return value != nullptr && value->kind == json::Kind::Literal && value->text == "null";
}

/** JsonPrimitive.float: content.toFloat() — String or bare literal, non-finite accepted,
 *  failures propagate to the caller's catch (modeled as nullopt here). */
std::optional<float> primitive_float(const json::Value& value) {
    if (value.kind != json::Kind::String && value.kind != json::Kind::Literal) {
        return std::nullopt;
    }
    const std::string content = value.text;
    const char* begin = content.c_str();
    char* end = nullptr;
    const float parsed = std::strtof(begin, &end);
    if (end != begin + content.size()) return std::nullopt;  // NumberFormatException
    return parsed;  // non-finite values are kept: Float.toString renders them as-is
}

/** kotlinx Float.toString spelling of one element (aborts the whole parse on failure). */
std::optional<std::string> float_text(const json::Value& value) {
    const auto parsed = primitive_float(value);
    if (!parsed) return std::nullopt;
    return openai::format_kotlin_float(*parsed);
}

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Request encoding
// ---------------------------------------------------------------------------------------------

std::string write_embedding_request(std::string_view text, std::string_view model) {
    std::string out;
    out += "{\"input\":";
    append_quoted(out, text);
    out += ",\"model\":";
    append_quoted(out, model);
    out += "}";
    return out;
}

std::string write_batch_embedding_request(const std::vector<std::string>& texts,
                                          std::string_view model) {
    std::string out;
    out += "{\"input\":[";
    for (std::size_t i = 0; i < texts.size(); ++i) {
        if (i > 0) out.push_back(',');
        append_quoted(out, texts[i]);
    }
    out += "],\"model\":";
    append_quoted(out, model);
    out += "}";
    return out;
}

// ---------------------------------------------------------------------------------------------
// Response parsing
// ---------------------------------------------------------------------------------------------

std::optional<std::string> parse_embedding_values(
    const std::optional<std::string_view>& response) {
    if (!response) return std::nullopt;
    const json::ParseResult parsed = json::parse(*response);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) return std::nullopt;

    const json::Value* data = member(*parsed.value, "data");
    if (data == nullptr || is_null(data) || data->kind != json::Kind::Array ||
        data->elements.empty()) {
        return std::nullopt;
    }
    const json::Value& first = data->elements.front();
    if (first.kind != json::Kind::Object) return std::nullopt;
    const json::Value* embedding = member(first, "embedding");
    if (embedding == nullptr || is_null(embedding) || embedding->kind != json::Kind::Array) {
        return std::nullopt;
    }
    std::string joined;
    for (const json::Value& element : embedding->elements) {
        const auto text = float_text(element);
        if (!text) return std::nullopt;
        if (!joined.empty()) joined.push_back(',');
        joined += *text;
    }
    return joined;
}

std::vector<std::optional<std::string>> parse_batch_embedding_entries(
    const std::optional<std::string_view>& response, std::size_t count) {
    // Kotlin checks texts.isEmpty() before touching the response.
    if (count == 0) return {};
    const auto all_nulls = [count] {
        return std::vector<std::optional<std::string>>(count, std::nullopt);
    };
    if (!response) return all_nulls();
    const json::ParseResult parsed = json::parse(*response);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) return all_nulls();
    const json::Value* data = member(*parsed.value, "data");
    if (data == nullptr || is_null(data) || data->kind != json::Kind::Array) return all_nulls();

    std::vector<std::optional<std::string>> entries;
    entries.reserve(data->elements.size());
    for (const json::Value& item : data->elements) {
        if (item.kind != json::Kind::Object) return all_nulls();  // item.jsonObject throws
        const json::Value* embedding = member(item, "embedding");
        if (embedding == nullptr) {
            entries.push_back(std::nullopt);  // absent key: the per-item miss
            continue;
        }
        // An explicit null is a PRESENT JsonNull, and JsonNull.jsonArray throws: all-nulls.
        if (is_null(embedding) || embedding->kind != json::Kind::Array) return all_nulls();
        std::string joined;
        for (const json::Value& element : embedding->elements) {
            const auto text = float_text(element);
            if (!text) return all_nulls();  // jsonPrimitive/toFloat failures escape the map
            if (!joined.empty()) joined.push_back(',');
            joined += *text;
        }
        entries.push_back(std::move(joined));
    }
    return entries;
}

}  // namespace agora::core::embedding
