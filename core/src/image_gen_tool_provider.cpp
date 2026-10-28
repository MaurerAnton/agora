#include "agora/core/image_gen_tool_provider.hpp"

#include <stdexcept>
#include <utility>

#include "agora/core/crypto_primitives.hpp"
#include "agora/core/json.hpp"

namespace agora::core::imagegen {

namespace {

namespace json = agora::core::json;

// ---- JSON construction (kotlinx buildJsonObject key order) ----------------------------------

json::Value json_string(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value json_number(std::string digits) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(digits);
    return value;
}

json::Value json_object(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

std::string render(const json::Value& value) { return json::to_kotlin_string(value); }

const json::Value* map_get(const json::Value& object, std::string_view key) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == key) found = &member.value;
    }
    return found;
}

/** `(element as? JsonPrimitive)?.content`. */
std::optional<std::string> primitive_content(const json::Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind == json::Kind::String || value->kind == json::Kind::Literal) return value->text;
    return std::nullopt;
}

/** `.jsonArray` — kotlinx throws on every other kind (JsonNull included). */
const json::Value& require_array(const json::Value* value) {
    if (value == nullptr || value->kind != json::Kind::Array) {
        throw std::runtime_error("Element is not a JsonArray");
    }
    return *value;
}

/** `.jsonObject` — kotlinx throws on every other kind (JsonNull included). */
const json::Value& require_object(const json::Value* value) {
    if (value == nullptr || value->kind != json::Kind::Object) {
        throw std::runtime_error("Element is not a JsonObject");
    }
    return *value;
}

/** Json.decodeFromString<Map<String, JsonElement>> for the response body (throws like it). */
json::Value parse_json_object(const std::string& source) {
    const json::ParseResult parsed = json::parse(source);
    if (!parsed.value) {
        throw std::runtime_error("Unexpected JSON token at offset " +
                                 std::to_string(parsed.error_position) + ": " + parsed.error);
    }
    if (parsed.value->kind != json::Kind::Object) {
        throw std::runtime_error("Expected start of the object '{'");
    }
    return *parsed.value;
}

/** Kotlin `err(code, message)` — message is omitted when null or blank. */
std::string err(const std::string& code, std::optional<std::string> message) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("type", json_string("image_generation"));
    members.emplace_back("error", json_string(code));
    if (message.has_value() && !json::is_blank(*message)) {
        members.emplace_back("message", json_string(std::move(*message)));
    }
    return render(json_object(std::move(members)));
}

openai::ToolProperty property(std::string type, std::string description) {
    openai::ToolProperty result;
    result.type = std::move(type);
    result.description = std::move(description);
    return result;
}

/** Kotlin String.trimEnd('/'). */
void trim_trailing_slashes(std::string& text) {
    while (!text.empty() && text.back() == '/') text.pop_back();
}

}  // namespace

ImageGenToolProvider::ImageGenToolProvider(HttpTransport& http, ImageStore& image_store)
    : http_(http), image_store_(image_store) {}

std::vector<openai::ToolDefinition> ImageGenToolProvider::definitions(
    const shell_provider::GenerationContext& ctx) const {
    if (!ctx.image_gen_enabled) return {};

    openai::ToolFunction function;
    function.name = "generate_image";
    function.description =
        "Generate an image from a text prompt. The generated image is shown to the user "
        "automatically — do NOT attempt to embed or describe the raw image data. Use this "
        "whenever the user asks to create, draw, paint, or generate a picture.";
    function.parameters.properties = {
        {"prompt", property("string", "A detailed description of the image to generate.")},
        {"size",
         property("string", "Optional image size, e.g. 1024x1024, 1024x1536, or 1536x1024.")}};
    function.parameters.required = {"prompt"};
    return {openai::ToolDefinition{"function", std::move(function)}};
}

bool ImageGenToolProvider::handles(const std::string& name) const {
    return name == "generate_image";
}

std::string ImageGenToolProvider::execute(const std::string& name, const std::string& arguments,
                                          const shell_provider::GenerationContext& ctx) {
    return execute_one_shot_result(name, arguments, ctx).text;
}

shell_provider::ToolExecutionResult ImageGenToolProvider::execute_one_shot_result(
    const std::string& name, const std::string& arguments,
    const shell_provider::GenerationContext& ctx) {
    std::pair<std::string, std::optional<shell_provider::ToolImageAttachment>> text_and_image =
        execute_off_main(name, arguments, ctx);
    const bool failed = !text_and_image.second.has_value();

    shell_provider::ToolExecutionResult result;
    result.text = std::move(text_and_image.first);
    if (text_and_image.second.has_value()) {
        result.images.push_back(std::move(*text_and_image.second));
    }
    result.is_error = failed;
    return result;
}

std::pair<std::string, std::optional<shell_provider::ToolImageAttachment>>
ImageGenToolProvider::execute_off_main(const std::string& /*name*/, const std::string& arguments,
                                       const shell_provider::GenerationContext& ctx) {
    if (!ctx.conversation_id.has_value() || json::is_blank(*ctx.conversation_id)) {
        return {err("missing_conversation", "Image generation requires a conversation."),
                std::nullopt};
    }

    json::Value args;
    args.kind = json::Kind::Object;
    const json::ParseResult parsed_args =
        json::parse(json::is_blank(arguments) ? "{}" : arguments);
    if (parsed_args.value && parsed_args.value->kind == json::Kind::Object) {
        args = *parsed_args.value;
    }

    const std::optional<std::string> prompt = primitive_content(map_get(args, "prompt"));
    if (!prompt.has_value()) return {err("no_prompt", std::nullopt), std::nullopt};

    std::optional<std::string> size = primitive_content(map_get(args, "size"));
    if (size.has_value() && json::is_blank(*size)) size.reset();
    if (!size.has_value()) {
        size = json::is_blank(ctx.image_gen_size) ? "1024x1024" : ctx.image_gen_size;
    }

    if (json::is_blank(ctx.image_gen_api_key)) {
        return {err("no_api_key", std::nullopt), std::nullopt};
    }

    std::string base_url =
        json::is_blank(ctx.image_gen_base_url) ? kOpenAiBaseUrl : ctx.image_gen_base_url;
    trim_trailing_slashes(base_url);
    const std::string model =
        json::is_blank(ctx.image_gen_model) ? "gpt-image-1" : ctx.image_gen_model;

    try {
        const std::string body = render(json_object({
            {"model", json_string(model)},
            {"prompt", json_string(*prompt)},
            {"size", json_string(*size)},
            {"n", json_number("1")},
        }));

        std::optional<std::string> response = http_.post_json(
            base_url + "/images/generations", body,
            {{"Authorization", "Bearer " + ctx.image_gen_api_key}}, kImageGenerationTimeoutMs,
            kImageGenerationTimeoutMs);
        if (!response.has_value()) return {err("no_response", std::nullopt), std::nullopt};

        const json::Value json_response = parse_json_object(*response);

        const json::Value* data = map_get(json_response, "data");
        const json::Value* first = nullptr;
        if (data != nullptr) {
            const json::Value& array = require_array(data);
            if (!array.elements.empty()) first = &require_object(&array.elements.front());
        }
        if (first == nullptr) {
            return {err("no_image", "The endpoint returned no image data."), std::nullopt};
        }

        std::string bytes;
        const std::optional<std::string> b64 = primitive_content(map_get(*first, "b64_json"));
        if (b64.has_value() && !json::is_blank(*b64)) {
            const crypto::Base64Decode decoded = crypto::base64_decode(*b64, /*url_safe=*/true);
            if (!decoded.bytes.has_value()) throw std::runtime_error(decoded.error);
            bytes.assign(decoded.bytes->begin(), decoded.bytes->end());
        } else {
            const std::optional<std::string> url = primitive_content(map_get(*first, "url"));
            if (!url.has_value()) {
                return {err("no_image", "No b64_json or url in the response."), std::nullopt};
            }
            std::optional<std::string> downloaded =
                http_.get_bytes(*url, kImageGenerationTimeoutMs, kImageGenerationTimeoutMs);
            if (!downloaded.has_value()) {
                return {err("download_failed", std::nullopt), std::nullopt};
            }
            bytes = std::move(*downloaded);
        }

        shell_provider::ToolImageAttachment attachment =
            image_store_.persist_generated_bytes(bytes, "generated_image");
        return {render(json_object({
                    {"type", json_string("image_generation")},
                    {"status", json_string("ok")},
                    {"size", json_string(*size)},
                })),
                std::move(attachment)};
    } catch (const std::exception& error) {
        return {err("generation_error", error.what()), std::nullopt};
    }
}

}  // namespace agora::core::imagegen
