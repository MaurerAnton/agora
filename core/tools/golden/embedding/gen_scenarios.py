#!/usr/bin/env python3
"""Generates the scenario corpus for the embedding golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json. Freeze the golden traces
afterwards with run_golden.sh --freeze.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def scenario(name, mode, **kw):
    out = {"name": name, "mode": mode}
    out.update(kw)
    ALL.append(out)


# ---- request encoding --------------------------------------------------------------------------------
scenario("request_single_special_chars", "requestSingle",
         text='Quote " newline \n backslash \\ euro €', model="text-embedding-3-small")
scenario("request_single_custom_model", "requestSingle",
         text="hi", model="voyage-3")
scenario("request_batch_special_chars", "requestBatch",
         texts=["first \"quoted\"", "line\nbreak", "tab\there"], model="text-embedding-3-small")
scenario("request_batch_empty_input", "requestBatch", texts=[], model="m")

# ---- single response parsing --------------------------------------------------------------------------
scenario("single_ok_extreme_floats", "single",
         response='{"data":[{"embedding":[0.1, -2.5, 1000000.0, 1e-8, -0.0, 1e999]}]}')
scenario("single_null_response", "single", response=None)
scenario("single_missing_response_field", "single")
scenario("single_empty_data", "single", response='{"data": []}')
scenario("single_missing_data", "single", response='{"model": "text-embedding-3-small"}')
scenario("single_data_wrong_kind", "single", response='{"data": {"0": 1}}')
scenario("single_item_not_object", "single", response='{"data": ["x"]}')
scenario("single_embedding_missing", "single", response='{"data": [{}]}')
scenario("single_embedding_wrong_kind", "single", response='{"data": [{"embedding": {}}]}')
scenario("single_element_not_primitive", "single",
         response='{"data": [{"embedding": [{}, 1]}]}')
scenario("single_non_numeric_element", "single",
         response='{"data": [{"embedding": ["abc"]}]}')
scenario("single_string_number_elements", "single",
         response='{"data": [{"embedding": ["1.5", "2"]}]}')
scenario("single_embedding_explicit_null", "single",
         response='{"data": [{"embedding": null}]}')
scenario("single_bad_json", "single", response="not-json{")
scenario("single_truncated_json", "single", response='{"data": [{"embedding": [1.0')

# ---- batch response parsing --------------------------------------------------------------------------
scenario("batch_ok", "batch",
         texts=["a", "b"],
         response='{"data":[{"embedding":[1.0,2.0]},{"embedding":[-3.5,4e2]}]}')
scenario("batch_null_response", "batch", texts=["a", "b"], response=None)
scenario("batch_missing_data", "batch", texts=["a", "b"], response='{"ok": true}')
scenario("batch_item_missing_embedding", "batch", texts=["a", "b"],
         response='{"data":[{"embedding":[1.0]},{}]}')
scenario("batch_item_not_object", "batch", texts=["a", "b"], response='{"data":["x"]}')
scenario("batch_embedding_explicit_null", "batch", texts=["a", "b"],
         response='{"data":[{"embedding":[1.0]},{"embedding":null}]}')
scenario("batch_element_not_primitive", "batch", texts=["a"],
         response='{"data":[{"embedding":[{"nested":1}]}]}')
scenario("batch_count_mismatch_follows_response", "batch", texts=["a", "b", "c"],
         response='{"data":[{"embedding":[1.0]},{"embedding":[2.0]}]}')
scenario("batch_empty_texts_skips_response", "batch", texts=[], response=None)
scenario("batch_bad_json", "batch", texts=["a"], response="<html>error</html>")


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
