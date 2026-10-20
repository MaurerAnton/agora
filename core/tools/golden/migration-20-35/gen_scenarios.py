#!/usr/bin/env python3
"""Generates the v20..v35 schema-migration scenarios. Deterministic."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

NAMES = {
    20: "m20_21_token_usage_columns",
    21: "m21_22_unread_generation",
    22: "m22_23_embedding_model_index",
    23: "m23_24_new_chat_persist",
    24: "m24_25_conversation_settings_transfer",
    25: "m25_26_cache_write_tokens",
    26: "m26_27_settings_import_transfer",
    27: "m27_28_maintenance_debt",
    28: "m28_29_semantic_index_tables",
    29: "m29_30_reconcile_revision",
    30: "m30_31_attachment_scan_index",
    31: "m31_32_generation_duration",
    32: "m32_33_data_changed_at",
    33: "m33_34_task_system_prompt_id",
    34: "m34_35_message_source_json",
}


def main() -> int:
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for version, name in sorted(NAMES.items()):
        path = os.path.join(SCENARIOS_DIR, f"{name}.json")
        with open(path, "w", encoding="utf-8") as stream:
            json.dump({"name": name, "op": "migrate", "version": version}, stream)
            stream.write("\n")
    print(f"wrote {len(NAMES)} scenarios into {SCENARIOS_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
