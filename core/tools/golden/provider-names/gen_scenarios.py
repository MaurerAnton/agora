#!/usr/bin/env python3
"""Generates the scenario corpus for the provider-names golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json. Freeze the golden traces
afterwards with run_golden.sh --freeze.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def allowed(name, provider):
    ALL.append({"name": name, "mode": "allowed", "providerName": provider})


def conflict(name, provider, existing=(), current=None):
    out = {"name": name, "mode": "conflict", "providerName": provider,
           "existingNames": list(existing)}
    if current is not None:
        out["currentName"] = current
    ALL.append(out)


def sanitize(name, configs):
    ALL.append({"name": name, "mode": "sanitize", "configs": list(configs)})


# ---- isAllowed ------------------------------------------------------------------------------
allowed("allowed_plain", "My Provider")
allowed("allowed_trimmed_padded", "  My Provider  ")
allowed("allowed_empty", "")
allowed("allowed_blank", "   ")
allowed("allowed_reserved_google", "Google")
allowed("allowed_reserved_lowercase", "google")
allowed("allowed_reserved_upper", "GOOGLE")
allowed("allowed_reserved_padded", "  Anthropic ")
allowed("allowed_reserved_openrouter", "open router")
allowed("allowed_reserved_opencode_go", "OpenCode Go")
allowed("allowed_reserved_local", "Local")
allowed("allowed_reserved_deepseek", "DeepSeek")
allowed("allowed_reserved_qwen", "Qwen")
allowed("allowed_reserved_groq", "Groq")
allowed("allowed_reserved_ollama", "Ollama")
allowed("allowed_reserved_openai", "OpenAI")
allowed("allowed_similar_to_reserved", "Googler")
allowed("allowed_reserved_with_inner_space", "Deep  Seek")
allowed("allowed_unicode", "Провайдер 🚀")

# ---- hasConflict ----------------------------------------------------------------------------
conflict("conflict_duplicate_case_insensitive", "Box", existing=["box"])
conflict("conflict_duplicate_trimmed", " Box ", existing=["Box"])
conflict("conflict_distinct", "Box", existing=["Bax"])
conflict("conflict_no_existing", "Box", existing=[])
conflict("conflict_disallowed_wins", "Google", existing=[])
conflict("conflict_disallowed_with_existing", "Google", existing=["Google Custom"])
conflict("conflict_current_entry_exempt", "Box", existing=["Box"], current="Box")
conflict("conflict_current_entry_case_sensitive", "Box", existing=["Box"], current="box")
conflict("conflict_current_entry_not_first", "Box", existing=["Other", "Box"], current="Box")
conflict("conflict_current_entry_absent", "Box", existing=["Box"], current=None)
conflict("conflict_current_entry_untrimmed", "Box", existing=[" Box "], current=" Box ")
conflict("conflict_blank_candidate", "   ", existing=["x"])
conflict("conflict_multiple_duplicates", "Box", existing=["box", "BOX"])

# ---- sanitize -------------------------------------------------------------------------------
sanitize("sanitize_empty", [])
sanitize("sanitize_single", ["Box"])
sanitize("sanitize_first_wins", ["Box", "box", " Google", "Local", "Foo"])
sanitize("sanitize_all_rejected", ["Google", "Local", ""])
sanitize("sanitize_all_accepted", ["Alpha", "Beta", "Gamma"])
sanitize("sanitize_trimmed_duplicates", ["Box", "  Box  ", "\tBox\n"])
sanitize("sanitize_blank_first", ["   ", "Box"])


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for item in ALL:
        path = os.path.join(SCENARIOS_DIR, item["name"] + ".json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(item, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    print(f"Wrote {len(ALL)} scenarios")


if __name__ == "__main__":
    main()