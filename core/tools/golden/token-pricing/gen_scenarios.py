#!/usr/bin/env python3
"""Generates the token-pricing scenarios. Deterministic; rerun after editing.

The cluster is ten host-compilable files under `api/util/tokens`: image descriptors and the byte
fallback, the two pixel rules, the Anthropic id parser and envelope table, the Gemini crop rule,
the OpenAI patch/tile table, the heuristic and local text counters, and the default bundle.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, **fields):
    ALL.append({"name": name, **fields})


def image(byte=None, width=None, height=None):
    return {"byteSize": byte, "pixelWidth": width, "pixelHeight": height}


def heuristic(name, text):
    case(name, op="heuristic", text=text)


def local(name, text, exact=None, throws=False):
    case(name, op="local", text=text, exact=exact, throws=throws)


def byte_image(name, desc):
    case(name, op="byteImage", image=desc)


def anthropic_tier(name, model):
    case(name, op="anthropicTier", model=model)


def anthropic_image(name, model, desc):
    case(name, op="anthropicImage", model=model, image=desc)


def gemini_image(name, desc):
    case(name, op="geminiImage", image=desc)


def openai_image(name, model, desc):
    case(name, op="openaiImage", model=model, image=desc)


def model_id(name, model):
    case(name, op="anthropicModelId", model=model)


def version(name, model, major, minor):
    case(name, op="versionAtLeast", model=model, major=major, minor=minor)


def envelope_anthropic(name, model):
    case(name, op="envelopeAnthropic", model=model)


def margin(name, raw):
    case(name, op="margin", raw=raw)


def default_model(name, text, desc, raw):
    case(name, op="defaultModel", text=text, image=desc, raw=raw)


# ---- HeuristicTextTokenCounter ----------------------------------------------------------------
heuristic("heuristic_empty", "")
heuristic("heuristic_ascii_short", "abcd")
heuristic("heuristic_ascii_ceil", "abcde")
heuristic("heuristic_two_words", "hello world")
heuristic("heuristic_mixed_punct", "hi!there")
heuristic("heuristic_cjk", "日本語")
heuristic("heuristic_emoji", "🙂🙂")
heuristic("heuristic_alnum", "abc123")
heuristic("heuristic_whitespace_only", "  \t\n")
heuristic("heuristic_accent", "café")
heuristic("heuristic_nbsp_not_whitespace", "a\u00a0b")
heuristic("heuristic_eight_run", "abcdefgh")
heuristic("heuristic_digit_run", "1234567")
heuristic("heuristic_three_runs", "ab cd ef")
heuristic("heuristic_newline_flush", "abc\ndef")
heuristic("heuristic_wide_mix", "aあb")

# ---- LocalModelTextCounter --------------------------------------------------------------------
local("local_empty_short_circuits", "", throws=True)
local("local_exact_used", "abcd", exact=5)
local("local_exact_zero", "abcd", exact=0)
local("local_exact_negative", "abcd", exact=-1)
local("local_missing_method", "abcd", throws=True)
local("local_heuristic_fallback", "日本語")
local("local_empty_negative", "", exact=-3)
local("local_long_run", "abcdefghij")

# ---- ByteProportionalImageCost ----------------------------------------------------------------
byte_image("byte_unknown", image())
byte_image("byte_zero", image(byte=0))
byte_image("byte_negative", image(byte=-5))
byte_image("byte_one_block", image(byte=768))
byte_image("byte_500", image(byte=384000))
byte_image("byte_1024", image(byte=786432))
byte_image("byte_1536", image(byte=1179648))
byte_image("byte_huge", image(byte=999999999))
byte_image("byte_just_below", image(byte=767))
byte_image("byte_255", image(byte=195840))
byte_image("byte_257", image(byte=197376))

# ---- AnthropicImageCost.tierForModelName ------------------------------------------------------
anthropic_tier("tier_46_standard", "claude-opus-4-6")
anthropic_tier("tier_47_high", "claude-opus-4-7")
anthropic_tier("tier_47_dated_high", "claude-sonnet-4-7-20260101")
anthropic_tier("tier_37_standard", "claude-3-7-sonnet-20250219")
anthropic_tier("tier_path_48_high", "anthropic/claude-opus-4-8")
anthropic_tier("tier_major_only_5", "claude-5")
anthropic_tier("tier_no_version", "claude")
anthropic_tier("tier_major_only_4", "claude-opus-4")
anthropic_tier("tier_two_digit_major", "claude-opus-14-7")
anthropic_tier("tier_dot_46", "claude-opus-4.6")
anthropic_tier("tier_dot_47", "claude-opus-4.7")
anthropic_tier("tier_haiku_45", "claude-haiku-4-5")
anthropic_tier("tier_release_date_only", "claude-opus-20260101")

# ---- AnthropicImageCost.tokens ----------------------------------------------------------------
anthropic_image("anthropic_square_100", "claude-opus-4-6", image(width=100, height=100))
anthropic_image("anthropic_standard_cap", "claude-opus-4-6", image(width=1568, height=1568))
anthropic_image("anthropic_high_cap", "claude-opus-4-7", image(width=2576, height=2576))
anthropic_image("anthropic_landscape", "claude-opus-4-6", image(width=1000, height=500))
anthropic_image("anthropic_long_edge_cap", "claude-opus-4-6", image(width=5000, height=1000))
anthropic_image("anthropic_byte_fallback", "claude-opus-4-6", image(byte=384000))
anthropic_image("anthropic_zero_dimension", "claude-opus-4-6",
                image(byte=786432, width=0, height=100))
anthropic_image("anthropic_high_small", "claude-opus-4-7", image(width=100, height=100))
anthropic_image("anthropic_portrait", "claude-opus-4-6", image(width=500, height=1000))
anthropic_image("anthropic_high_landscape", "claude-opus-4-7",
                image(width=3000, height=1000))
anthropic_image("anthropic_standard_wide", "claude-opus-4-6", image(width=2500, height=1000))

# ---- GeminiImageCost --------------------------------------------------------------------------
gemini_image("gemini_single_384", image(width=384, height=384))
gemini_image("gemini_single_100", image(width=100, height=100))
gemini_image("gemini_over_width", image(width=385, height=384))
gemini_image("gemini_over_height", image(width=384, height=385))
gemini_image("gemini_guide_960x540", image(width=960, height=540))
gemini_image("gemini_one_px", image(width=1, height=1))
gemini_image("gemini_tall", image(width=540, height=960))
gemini_image("gemini_byte_fallback", image(byte=786432))
gemini_image("gemini_zero_dimension", image(byte=384000, width=0, height=500))
gemini_image("gemini_large", image(width=1920, height=1080))
gemini_image("gemini_crop_unit_one", image(width=1000, height=1))
# 4:3 ratios whose tile count flips if floor(min / 1.5) becomes anything else: 300/1.5 = 200
# tiles the width twice, 300/1.6 = 187 tiles it three times.
gemini_image("gemini_landscape_4_3", image(width=400, height=300))
gemini_image("gemini_portrait_4_3", image(width=300, height=400))
gemini_image("gemini_wide_4_3", image(width=800, height=600))

# ---- OpenAiImageCosts -------------------------------------------------------------------------
openai_image("openai_default_small", "totally-unknown", image(width=100, height=100))
openai_image("openai_gpt4o_small", "gpt-4o", image(width=100, height=100))
openai_image("openai_gpt4o_mini_1024", "gpt-4o-mini", image(width=1024, height=1024))
openai_image("openai_gpt4o_mini_path", "openai/gpt-4o-mini", image(width=100, height=100))
openai_image("openai_gpt41_small", "gpt-4.1", image(width=100, height=100))
openai_image("openai_gpt41_mini_small", "gpt-4.1-mini", image(width=100, height=100))
openai_image("openai_gpt41_nano_small", "gpt-4.1-nano", image(width=100, height=100))
openai_image("openai_gpt41_mini_square", "gpt-4.1-mini", image(width=10000, height=10000))
openai_image("openai_gpt51_1000", "gpt-5.1", image(width=1000, height=1000))
openai_image("openai_gpt52_square", "gpt-5.2", image(width=5000, height=5000))
openai_image("openai_gpt54_shrink_square", "gpt-5.4", image(width=5000, height=5000))
openai_image("openai_gpt54_shrink_2048", "gpt-5.4", image(width=2048, height=2048))
# Non-square shrink inputs: the shrink correction (whole-patch truncation) changes the scaled
# dimensions here, while the square cases shrink to exact integer patches where it is 1.
openai_image("openai_gpt54_shrink_landscape", "gpt-5.4", image(width=5000, height=4000))
openai_image("openai_gpt55_shrink_landscape", "gpt-5.5", image(width=10000, height=7000))
openai_image("openai_gpt55_shrink", "gpt-5.5", image(width=10000, height=10000))
openai_image("openai_gpt5nano_small", "gpt-5-nano", image(width=100, height=100))
openai_image("openai_gpt5mini_small", "gpt-5-mini", image(width=100, height=100))
openai_image("openai_gpt5_1000", "gpt-5", image(width=1000, height=1000))
openai_image("openai_o4_mini_small", "o4-mini", image(width=100, height=100))
openai_image("openai_o1_small", "o1-preview", image(width=100, height=100))
openai_image("openai_o3_square", "o3-mini", image(width=5000, height=5000))
openai_image("openai_gpt4o_wide_fit", "gpt-4o", image(width=3000, height=1000))
openai_image("openai_byte_fallback", "gpt-4o", image(byte=786432))
openai_image("openai_unknown_huge", "mystery-vision", image(width=70000, height=70000))

# ---- AnthropicModelId -------------------------------------------------------------------------
model_id("model_id_opus_45", "claude-opus-4-5-20251101")
model_id("model_id_path_37", "anthropic/claude-3-7-sonnet-20250219")
model_id("model_id_dot_46", "claude-opus-4.6")
model_id("model_id_sonnet_4", "claude-sonnet-4")
model_id("model_id_haiku_35", "claude-haiku-3.5")
model_id("model_id_other_vendor", "gpt-4-5")
model_id("model_id_no_version", "claude-opus")
model_id("model_id_release_date_only", "claude-opus-20260101")
model_id("model_id_mid_version", "claude-3-opus-4-5")
model_id("model_id_upper", "CLAUDE-OPUS-4-7")
model_id("model_id_trailing_dot_version", "claude-opus-4.5.1")
model_id("model_id_long_major_run", "claude-opus-123-4")
model_id("model_id_long_minor_run", "claude-opus-4-123456")
model_id("model_id_two_digit_pair", "claude-opus-14.11")

# ---- AnthropicModelId.versionAtLeast ----------------------------------------------------------
version("version_at_least_equal", "claude-opus-4-7", 4, 7)
version("version_at_least_minor_short", "claude-opus-4-7", 4, 8)
version("version_at_least_major_short", "claude-opus-4-7", 5, 0)
version("version_at_least_major_over", "claude-opus-5", 4, 9)
version("version_at_least_unreadable", "claude", 4, 0)
version("version_at_least_major_only", "claude-opus-4", 4, 1)
version("version_at_least_dot", "claude-opus-4.6", 4, 6)
version("version_at_least_old", "claude-opus-3-9", 4, 0)

# ---- Envelope costs ---------------------------------------------------------------------------
case("envelope_default", op="envelopeDefault")
case("envelope_openai", op="envelopeOpenAi")
envelope_anthropic("envelope_opus_45", "claude-opus-4-5")
envelope_anthropic("envelope_opus_46", "claude-opus-4-6")
envelope_anthropic("envelope_opus_47", "claude-opus-4-7")
envelope_anthropic("envelope_opus_48", "claude-opus-4-8")
envelope_anthropic("envelope_opus_5", "claude-opus-5")
envelope_anthropic("envelope_opus_55", "claude-opus-5-5")
envelope_anthropic("envelope_sonnet_5", "claude-sonnet-5")
envelope_anthropic("envelope_sonnet_46", "claude-sonnet-4-6")
envelope_anthropic("envelope_haiku_45", "claude-haiku-4-5")
envelope_anthropic("envelope_haiku_35", "claude-haiku-3-5")
envelope_anthropic("envelope_opus_41", "claude-opus-4-1")
envelope_anthropic("envelope_opus_40", "claude-opus-4-0")
envelope_anthropic("envelope_sonnet_40", "claude-sonnet-4-0")
envelope_anthropic("envelope_unknown_vendor", "gpt-4o")
envelope_anthropic("envelope_unpublished", "claude-opus-4-9")
envelope_anthropic("envelope_future_major", "claude-opus-6-0")
envelope_anthropic("envelope_upper", "CLAUDE-OPUS-4-8")

# ---- SafetyMargin -----------------------------------------------------------------------------
margin("margin_zero", 0)
margin("margin_one", 1)
margin("margin_two", 2)
margin("margin_nine", 9)
margin("margin_ten", 10)
margin("margin_99", 99)
margin("margin_100", 100)
margin("margin_int_max", 2147483647)
margin("margin_over_int_max", 2000000000)
margin("margin_negative", -5)
margin("margin_negative_one", -1)
margin("margin_large_in_range", 195000000)

# ---- ContextCostModel.Default wiring ----------------------------------------------------------
default_model("default_model_text", "abcd", image(byte=786432), 100)
default_model("default_model_cjk", "日本語", image(), 1)
default_model("default_model_empty", "", image(byte=384000), 0)
default_model("default_model_margin_clamp", "abcdefgh", image(byte=195840), 195000000)


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for existing in os.listdir(SCENARIOS_DIR):
        if existing.endswith(".json"):
            os.remove(os.path.join(SCENARIOS_DIR, existing))
    for scenario in ALL:
        path = os.path.join(SCENARIOS_DIR, scenario["name"] + ".json")
        with open(path, "w", encoding="utf-8") as stream:
            json.dump(scenario, stream, ensure_ascii=False, separators=(",", ":"))
            stream.write("\n")
    print(f"wrote {len(ALL)} scenarios to {SCENARIOS_DIR}")


if __name__ == "__main__":
    main()
