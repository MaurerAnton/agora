// Unit tests for the ImageGenToolProvider port: the imageGenEnabled gate, the exact
// generate_image definition, the argument decoding with JsonNull quirks, the request body
// field order, the response walk (data array, b64_json vs url fallback), the base64 decode
// via the vendored platform double, the success/error envelopes, and the image store seam.

#include "agora/core/image_gen_tool_provider.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"

namespace imagegen = agora::core::imagegen;
namespace shell_provider = agora::core::shell_provider;

namespace {

// ---------------------------------------------------------------------------------------------
// Scripted HttpTransport
// ---------------------------------------------------------------------------------------------

struct ScriptedHttp final : imagegen::HttpTransport {
    std::vector<std::string> posted_urls;
    std::vector<std::string> posted_bodies;
    std::vector<std::vector<std::pair<std::string, std::string>>> posted_headers;
    std::vector<std::string> gotten_urls;
    std::optional<std::string> next_post_response;
    std::optional<std::string> next_get_response;
    bool post_should_throw = false;
    bool get_should_throw = false;

    std::optional<std::string> post_json(
        const std::string& url, const std::string& body,
        const std::vector<std::pair<std::string, std::string>>& headers,
        std::int64_t /*call_timeout_ms*/, std::int64_t /*read_timeout_ms*/) override {
        if (post_should_throw) throw std::runtime_error("http post failed");
        posted_urls.push_back(url);
        posted_bodies.push_back(body);
        posted_headers.push_back(headers);
        return next_post_response;
    }

    std::optional<std::string> get_bytes(
        const std::string& url, std::int64_t /*call_timeout_ms*/,
        std::int64_t /*read_timeout_ms*/) override {
        if (get_should_throw) throw std::runtime_error("http get failed");
        gotten_urls.push_back(url);
        return next_get_response;
    }
};

// ---------------------------------------------------------------------------------------------
// Scripted ImageStore
// ---------------------------------------------------------------------------------------------

struct ScriptedImageStore final : imagegen::ImageStore {
    std::vector<std::string> persisted_bytes;
    std::vector<std::string> persisted_prefixes;
    shell_provider::ToolImageAttachment next_attachment;

    shell_provider::ToolImageAttachment persist_generated_bytes(
        const std::string& bytes, const std::string& file_prefix) override {
        persisted_bytes.push_back(bytes);
        persisted_prefixes.push_back(file_prefix);
        return next_attachment;
    }
};

// ---------------------------------------------------------------------------------------------
// Fixture and helpers
// ---------------------------------------------------------------------------------------------

struct Fixture {
    ScriptedHttp http;
    ScriptedImageStore store;
    imagegen::ImageGenToolProvider provider;
    shell_provider::GenerationContext ctx;

    Fixture() : provider(http, store) {
        ctx.image_gen_enabled = true;
        ctx.image_gen_api_key = "test-key";
        ctx.image_gen_base_url = "https://api.openai.com/v1";
        ctx.image_gen_model = "gpt-image-1";
        ctx.image_gen_size = "1024x1024";
        ctx.conversation_id = "conv-123";

        store.next_attachment = shell_provider::ToolImageAttachment{
            "/tmp/generated_image_abc.png",
            "image/png",
            12345,
            1024,
            1024,
            "deadbeef"
        };
    }
};

shell_provider::GenerationContext disabled_ctx() {
    shell_provider::GenerationContext ctx;
    ctx.image_gen_enabled = false;
    return ctx;
}

std::string valid_b64() {
    // "test" encoded with standard base64
    return "dGVzdA==";
}

std::string response_with_b64(const std::string& b64) {
    return R"({"data":[{"b64_json":")" + b64 + R"("}]})";
}

std::string response_with_url(const std::string& url) {
    return R"({"data":[{"url":")" + url + R"("}]})";
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------

TEST(image_gen_definitions_gated_by_image_gen_enabled) {
    Fixture f;
    CHECK(f.provider.definitions(disabled_ctx()).empty());

    const auto defs = f.provider.definitions(f.ctx);
    CHECK_EQ(std::size_t(1), defs.size());
    CHECK_EQ(std::string("generate_image"), defs[0].function.name);
    CHECK(f.provider.handles("generate_image"));
    CHECK(!f.provider.handles("other_tool"));
}

TEST(image_gen_definition_exact_shape) {
    Fixture f;
    const auto defs = f.provider.definitions(f.ctx);
    const auto& fn = defs[0].function;

    CHECK_EQ(std::string("function"), defs[0].type);
    CHECK_EQ(std::string("Generate an image from a text prompt. The generated image is shown to "
                         "the user automatically — do NOT attempt to embed or describe the raw "
                         "image data. Use this whenever the user asks to create, draw, paint, or "
                         "generate a picture."),
             fn.description);
    CHECK_EQ(std::size_t(2), fn.parameters.properties.size());
    CHECK_EQ(std::string("prompt"), fn.parameters.properties[0].first);
    CHECK_EQ(std::string("string"), fn.parameters.properties[0].second.type);
    CHECK_EQ(std::string("A detailed description of the image to generate."),
             fn.parameters.properties[0].second.description);
    CHECK_EQ(std::string("size"), fn.parameters.properties[1].first);
    CHECK_EQ(std::string("string"), fn.parameters.properties[1].second.type);
    CHECK_EQ(std::string("Optional image size, e.g. 1024x1024, 1024x1536, or 1536x1024."),
             fn.parameters.properties[1].second.description);
    CHECK(fn.parameters.required == std::vector<std::string>{"prompt"});
}

TEST(image_gen_missing_conversation_id) {
    Fixture f;
    f.ctx.conversation_id.reset();
    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"a cat"})", f.ctx);
    CHECK_EQ(std::string(R"({"type":"image_generation","error":"missing_conversation","message":"Image generation requires a conversation."})"),
             result.text);
    CHECK(result.is_error);
}

TEST(image_gen_blank_conversation_id) {
    Fixture f;
    f.ctx.conversation_id = "   ";
    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"a cat"})", f.ctx);
    CHECK_EQ(std::string(R"({"type":"image_generation","error":"missing_conversation","message":"Image generation requires a conversation."})"),
             result.text);
}

TEST(image_gen_no_prompt) {
    Fixture f;
    const auto result = f.provider.execute_one_shot_result("generate_image", "{}", f.ctx);
    CHECK_EQ(std::string(R"({"type":"image_generation","error":"no_prompt"})"), result.text);
    CHECK(result.is_error);
}

TEST(image_gen_prompt_json_null_becomes_literal_null_string) {
    Fixture f;
    f.http.next_post_response = response_with_b64(valid_b64());
    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":null})", f.ctx);
    // JsonNull is a JsonPrimitive with content "null", so prompt becomes "null" string
    CHECK(!result.is_error);
    // Verify the request body contains the literal string "null" as the prompt value
    CHECK(f.http.posted_bodies[0].find(R"("prompt":"null")") != std::string::npos);
}

TEST(image_gen_size_uses_fallbacks) {
    Fixture f;
    // size from args
    f.http.next_post_response = response_with_b64(valid_b64());
    f.provider.execute_one_shot_result("generate_image", R"({"prompt":"x","size":"512x512"})", f.ctx);
    CHECK_EQ(std::size_t(1), f.http.posted_bodies.size());
    CHECK(f.http.posted_bodies[0].find(R"("size":"512x512")") != std::string::npos);

    // blank size arg → ctx.imageGenSize
    f.http.next_post_response = response_with_b64(valid_b64());
    f.provider.execute_one_shot_result("generate_image", R"({"prompt":"x","size":""})", f.ctx);
    CHECK(f.http.posted_bodies.back().find(R"("size":"1024x1024")") != std::string::npos);

    // no size arg → ctx.imageGenSize
    f.http.next_post_response = response_with_b64(valid_b64());
    f.provider.execute_one_shot_result("generate_image", R"({"prompt":"x"})", f.ctx);
    CHECK(f.http.posted_bodies.back().find(R"("size":"1024x1024")") != std::string::npos);
}

TEST(image_gen_request_body_field_order_and_values) {
    Fixture f;
    f.http.next_post_response = response_with_b64(valid_b64());

    f.provider.execute_one_shot_result("generate_image", R"({"prompt":"hello world"})", f.ctx);

    CHECK_EQ(std::size_t(1), f.http.posted_urls.size());
    CHECK_EQ(std::string("https://api.openai.com/v1/images/generations"), f.http.posted_urls[0]);
    CHECK_EQ(std::size_t(1), f.http.posted_headers.size());
    CHECK_EQ(std::string("Authorization"), f.http.posted_headers[0][0].first);
    CHECK_EQ(std::string("Bearer test-key"), f.http.posted_headers[0][0].second);

    // compact JSON with fields in declared order: model, prompt, size, n
    const std::string expected = R"({"model":"gpt-image-1","prompt":"hello world","size":"1024x1024","n":1})";
    CHECK_EQ(expected, f.http.posted_bodies[0]);
}

TEST(image_gen_base_url_trim_trailing_slash) {
    Fixture f;
    f.ctx.image_gen_base_url = "https://api.example.com/v1/";
    f.http.next_post_response = response_with_b64(valid_b64());

    f.provider.execute_one_shot_result("generate_image", R"({"prompt":"x"})", f.ctx);
    CHECK_EQ(std::string("https://api.example.com/v1/images/generations"), f.http.posted_urls[0]);
}

TEST(image_gen_model_fallback) {
    Fixture f;
    f.ctx.image_gen_model = "";
    f.http.next_post_response = response_with_b64(valid_b64());

    f.provider.execute_one_shot_result("generate_image", R"({"prompt":"x"})", f.ctx);
    CHECK(f.http.posted_bodies[0].find(R"("model":"gpt-image-1")") != std::string::npos);
}

TEST(image_gen_no_api_key) {
    Fixture f;
    f.ctx.image_gen_api_key = "";
    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"x"})", f.ctx);
    CHECK_EQ(std::string(R"({"type":"image_generation","error":"no_api_key"})"), result.text);
}

TEST(image_gen_http_post_returns_nullopt_is_no_response) {
    Fixture f;
    f.http.next_post_response = std::nullopt;
    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"x"})", f.ctx);
    CHECK_EQ(std::string(R"({"type":"image_generation","error":"no_response"})"), result.text);
}

TEST(image_gen_response_missing_data) {
    Fixture f;
    f.http.next_post_response = R"({})";
    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"x"})", f.ctx);
    CHECK_EQ(std::string(R"({"type":"image_generation","error":"no_image","message":"The endpoint returned no image data."})"),
             result.text);
}

TEST(image_gen_response_data_null_throws) {
    Fixture f;
    f.http.next_post_response = R"({"data":null})";
    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"x"})", f.ctx);
    // JsonNull is a Literal; require_array throws "Element is not a JsonArray"
    CHECK(result.text.find("Element is not a JsonArray") != std::string::npos);
    CHECK(result.is_error);
}

TEST(image_gen_response_data_empty_array_is_no_image) {
    Fixture f;
    f.http.next_post_response = R"({"data":[]})";
    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"x"})", f.ctx);
    CHECK_EQ(std::string(R"({"type":"image_generation","error":"no_image","message":"The endpoint returned no image data."})"),
             result.text);
}

TEST(image_gen_first_data_element_not_object_throws) {
    Fixture f;
    f.http.next_post_response = R"({"data":["not-an-object"]})";
    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"x"})", f.ctx);
    CHECK(result.text.find("Element is not a JsonObject") != std::string::npos);
    CHECK(result.is_error);
}

TEST(image_gen_b64_json_decoded_and_persisted) {
    Fixture f;
    f.http.next_post_response = response_with_b64(valid_b64());

    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"a cat"})", f.ctx);

    // success envelope
    CHECK_EQ(std::string(R"({"type":"image_generation","status":"ok","size":"1024x1024"})"),
             result.text);
    CHECK(!result.is_error);
    CHECK_EQ(std::size_t(1), result.images.size());
    CHECK_EQ(std::string("/tmp/generated_image_abc.png"), result.images[0].path);

    // image store received the decoded bytes ("test")
    CHECK_EQ(std::size_t(1), f.store.persisted_bytes.size());
    CHECK_EQ(std::string("test"), f.store.persisted_bytes[0]);
    CHECK_EQ(std::string("generated_image"), f.store.persisted_prefixes[0]);
}

TEST(image_gen_b64_json_blank_falls_back_to_url) {
    Fixture f;
    f.http.next_post_response = response_with_url("https://cdn.example.com/img.png");
    f.http.next_get_response = "raw-image-bytes";

    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"x"})", f.ctx);

    CHECK(!result.is_error);
    CHECK_EQ(std::size_t(1), f.http.gotten_urls.size());
    CHECK_EQ(std::string("https://cdn.example.com/img.png"), f.http.gotten_urls[0]);
    CHECK_EQ(std::string("raw-image-bytes"), f.store.persisted_bytes[0]);
}

TEST(image_gen_b64_json_present_but_empty_string_falls_back_to_url) {
    Fixture f;
    f.http.next_post_response = R"({"data":[{"b64_json":"","url":"https://cdn.example.com/img.png"}]})";
    f.http.next_get_response = "url-bytes";

    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"x"})", f.ctx);

    CHECK(!result.is_error);
    CHECK_EQ(std::size_t(1), f.http.gotten_urls.size());
    CHECK_EQ(std::string("url-bytes"), f.store.persisted_bytes[0]);
}

TEST(image_gen_b64_json_and_url_missing) {
    Fixture f;
    f.http.next_post_response = R"({"data":[{"foo":"bar"}]})";

    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"x"})", f.ctx);
    CHECK_EQ(std::string(R"({"type":"image_generation","error":"no_image","message":"No b64_json or url in the response."})"),
             result.text);
}

TEST(image_gen_download_failed) {
    Fixture f;
    f.http.next_post_response = response_with_url("https://cdn.example.com/img.png");
    f.http.next_get_response = std::nullopt;

    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"x"})", f.ctx);
    CHECK_EQ(std::string(R"({"type":"image_generation","error":"download_failed"})"), result.text);
}

TEST(image_gen_b64_decode_error_surfaces_as_generation_error) {
    Fixture f;
    // "!!!" is invalid base64 → crypto::base64_decode returns error
    f.http.next_post_response = R"({"data":[{"b64_json":"!!!"}]})";

    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"x"})", f.ctx);
    CHECK(result.text.find(R"({"type":"image_generation","error":"generation_error")") == 0);
    CHECK(result.text.find("Illegal base64 character") != std::string::npos);
}

TEST(image_gen_http_exception_surfaces_as_generation_error) {
    Fixture f;
    f.http.post_should_throw = true;

    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"x"})", f.ctx);
    CHECK(result.text.find(R"({"type":"image_generation","error":"generation_error")") == 0);
    CHECK(result.text.find("http post failed") != std::string::npos);
}

TEST(image_gen_store_exception_surfaces_as_generation_error) {
    Fixture f;
    f.http.next_post_response = response_with_b64(valid_b64());
    struct ThrowingStore : imagegen::ImageStore {
        shell_provider::ToolImageAttachment persist_generated_bytes(
            const std::string&, const std::string&) override {
            throw std::runtime_error("disk full");
        }
    };
    ThrowingStore throwing;
    imagegen::ImageGenToolProvider provider(f.http, throwing);

    const auto result = provider.execute_one_shot_result("generate_image",
                                                         R"({"prompt":"x"})", f.ctx);
    CHECK(result.text.find(R"({"type":"image_generation","error":"generation_error")") == 0);
    CHECK(result.text.find("disk full") != std::string::npos);
}

TEST(image_gen_execute_is_text_adapter) {
    Fixture f;
    f.http.next_post_response = response_with_b64(valid_b64());

    const auto one_shot = f.provider.execute_one_shot_result("generate_image",
                                                             R"({"prompt":"x"})", f.ctx);
    const auto direct = f.provider.execute("generate_image", R"({"prompt":"x"})", f.ctx);

    CHECK_EQ(one_shot.text, direct);
    CHECK(!one_shot.is_error);
}

TEST(image_gen_malformed_response_json_is_generation_error) {
    Fixture f;
    f.http.next_post_response = "not json";

    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"x"})", f.ctx);
    CHECK(result.text.find(R"({"type":"image_generation","error":"generation_error")") == 0);
    CHECK(result.text.find("Unexpected JSON token") != std::string::npos);
}

TEST(image_gen_non_object_response_is_generation_error) {
    Fixture f;
    f.http.next_post_response = "[]";

    const auto result = f.provider.execute_one_shot_result("generate_image",
                                                           R"({"prompt":"x"})", f.ctx);
    CHECK(result.text.find(R"({"type":"image_generation","error":"generation_error")") == 0);
    CHECK(result.text.find("Expected start of the object") != std::string::npos);
}