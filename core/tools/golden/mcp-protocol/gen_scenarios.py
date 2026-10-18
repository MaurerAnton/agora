#!/usr/bin/env python3
"""Generates the scenario corpus for the mcp-protocol golden stand.

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


EMPTY_SCHEMA = {"type": "object", "properties": {}}


# ---- publicMcpToolName --------------------------------------------------------------------------------
scenario("name_plain", "toolName", serverId="srv1", remoteName="search")
scenario("name_filtered_server", "toolName", serverId="srv!!-1__", remoteName="search")
scenario("name_blank_server_fallback", "toolName", serverId="!!!", remoteName="search")
scenario("name_symbols_mapped", "toolName", serverId="s1", remoteName="fs.read_file!@#")
scenario("name_underscore_trimmed", "toolName", serverId="s1", remoteName="__tool__")
scenario("name_blank_tool_fallback", "toolName", serverId="s1", remoteName="***")
scenario("name_long_remote_truncation", "toolName", serverId="s1",
         remoteName="a" * 80 + "z")
scenario("name_long_server_truncation", "toolName", serverId="s" * 40,
         remoteName="read")
scenario("name_unicode_kept", "toolName", serverId="сервер", remoteName="поиск")
scenario("name_emoji_tool", "toolName", serverId="s1", remoteName="a😀b")
scenario("name_spaces", "toolName", serverId="my server", remoteName="my tool")


# ---- asToolDefinition + schema projections -----------------------------------------------------------------
scenario("definition_empty_schema", "toolDefinition", remoteName="noop",
         description="", inputSchema=dict(EMPTY_SCHEMA))
scenario("definition_blank_description_default", "toolDefinition", remoteName="noop",
         description="   ", serverName="Box", inputSchema=dict(EMPTY_SCHEMA))
scenario("definition_sorted_properties", "toolDefinition", remoteName="t",
         inputSchema={
             "type": "object",
             "properties": {"zeta": {"type": "string"}, "alpha": {"type": "number"},
                            "mid": {"type": "boolean"}},
             "required": ["zeta", "alpha", "missing"],
         })
scenario("definition_required_non_primitives", "toolDefinition", remoteName="t",
         inputSchema={
             "type": "object",
             "properties": {"a": {"type": "string"}},
             "required": ["a", {"x": 1}, [1, 2], 7, None, "zz"],
         })
scenario("definition_type_array_union", "toolDefinition", remoteName="t",
         inputSchema={
             "type": ["null", "string"],
             "properties": {
                 "a": {"type": ["null", "integer"], "description": "A"},
                 "b": {"type": ["string"]},
                 "c": {},
                 "d": {"type": 5},
             },
         })
scenario("definition_nested_items", "toolDefinition", remoteName="t",
         inputSchema={
             "type": "object",
             "properties": {
                 "list": {"type": "array", "items": {"type": "integer", "description": "n"}},
                 "obj": {"properties": {"inner": {"type": "string"}}},
                 "deep": {"items": {"items": {"type": "boolean"}}},
                 "none": None,
                 "arr_prop": {"type": "string", "properties": {"p": {"type": "string"}},
                              "required": ["p"]},
             },
         })
scenario("definition_schema_marker_dropped", "toolDefinition", remoteName="t",
         inputSchema={
             "$schema": "https://json-schema.org/draft/2020-12/schema",
             "title": "T",
             "type": "object",
             "properties": {"a": {"type": "string"}},
             "required": ["a", "ghost"],
         })
scenario("definition_type_missing_defaults_object", "toolDefinition", remoteName="t",
         inputSchema={"properties": {"a": {"type": "string"}}})
scenario("definition_required_absent", "toolDefinition", remoteName="t",
         inputSchema={"type": "object", "required": "a", "properties": {"a": {"type": "string"}}})
scenario("definition_properties_not_object", "toolDefinition", remoteName="t",
         inputSchema={"type": "object", "properties": [1, 2], "required": ["a"]})
scenario("definition_description_kept", "toolDefinition", remoteName="t",
         description="Does a thing.", inputSchema={
             "type": "object", "properties": {"a": {"type": "string", "description": "An a."}},
             "required": ["a"]})


# ---- parseCallPayload -------------------------------------------------------------------------------------
scenario("payload_text_only", "callPayload",
         result={"content": [{"type": "text", "text": "hello"}, {"type": "text", "text": " world"}]})
scenario("payload_image", "callPayload",
         result={"content": [{"type": "image", "data": "AAAA", "mimeType": "image/png"}]})
scenario("payload_image_incomplete", "callPayload",
         result={"content": [
             {"type": "image", "data": "", "mimeType": "image/png"},
             {"type": "image", "data": "BBBB", "mimeType": "  "},
             {"type": "image", "data": "CCCC"},
         ]})
scenario("payload_resource_image", "callPayload",
         result={"content": [
             {"type": "resource", "resource": {"mimeType": "image/gif", "blob": "ZZ"}},
         ]})
scenario("payload_resource_text", "callPayload",
         result={"content": [
             {"type": "resource", "resource": {"mimeType": "text/plain", "text": "inline"}},
         ]})
scenario("payload_resource_blank_blob_falls_back", "callPayload",
         result={"content": [
             {"type": "resource", "resource": {"mimeType": "image/png", "blob": " ", "text": "T"}},
         ]})
scenario("payload_resource_missing", "callPayload",
         result={"content": [{"type": "resource"}, {"type": "resource", "resource": 5}]})
scenario("payload_structured_and_error", "callPayload",
         result={"content": [], "structuredContent": {"a": 1}, "isError": True})
scenario("payload_structured_null", "callPayload",
         result={"structuredContent": None, "isError": "true"})
scenario("payload_is_error_strict", "callPayload", result={"isError": "TRUE"})
scenario("payload_unknown_types", "callPayload",
         result={"content": [{"type": "video"}, {"type": None}, "x", 5, {"text": "no type"}]})
scenario("payload_content_not_array", "callPayload", result={"content": {"type": "text"}})
scenario("payload_empty", "callPayload", result={})


# ---- tools/list page mapping -----------------------------------------------------------------------------
scenario("page_basic", "toolsPage",
         result={"tools": [{"name": "a", "description": "A", "inputSchema": {"type": "object"}},
                           {"name": "b"}],
                 "nextCursor": "c2"})
scenario("page_skips_invalid", "toolsPage",
         result={"tools": ["x", 5, {"name": "  "}, {"name": "ok"}, {}],
                 "nextCursor": "   "})
scenario("page_default_schema", "toolsPage",
         result={"tools": [{"name": "a", "inputSchema": None},
                           {"name": "b", "inputSchema": 7}]})
scenario("page_missing_tools_array", "toolsPage", result={"nextCursor": "c"})
scenario("page_description_blank_kept", "toolsPage",
         result={"tools": [{"name": "a", "description": None}]})


# ---- JSON-RPC response validation ---------------------------------------------------------------------------
scenario("rpc_success", "rpcResponse", method="tools/call", id=7,
         response={"jsonrpc": "2.0", "id": "7", "result": {"ok": True}})
scenario("rpc_empty_response", "rpcResponse", method="tools/call", id=7)
scenario("rpc_id_number_mismatch", "rpcResponse", method="tools/list", id=3,
         response={"id": "4", "result": {}})
scenario("rpc_id_missing", "rpcResponse", method="tools/list", id=3, response={"result": {}})
scenario("rpc_error_object", "rpcResponse", method="tools/call", id=2,
         response={"id": "2", "error": {"message": "boom", "code": -32000}})
scenario("rpc_error_object_without_message", "rpcResponse", method="tools/call", id=2,
         response={"id": "2", "error": {"code": -1}})
scenario("rpc_error_primitive", "rpcResponse", method="tools/call", id=2,
         response={"id": "2", "error": "plain failure"})
scenario("rpc_error_null_ignored", "rpcResponse", method="tools/call", id=2,
         response={"id": "2", "error": None, "result": {"a": 1}})
scenario("rpc_result_missing", "rpcResponse", method="initialize", id=1, response={"id": "1"})
scenario("rpc_result_not_object", "rpcResponse", method="initialize", id=1,
         response={"id": "1", "result": [1, 2]})


# ---- negotiation -------------------------------------------------------------------------------------------
scenario("negotiate_current_head", "negotiation", transportType="STREAMABLE_HTTP",
         currentVersion="2025-11-25")
scenario("negotiate_middle_version", "negotiation", transportType="STREAMABLE_HTTP",
         currentVersion="2025-03-26")
scenario("negotiate_unknown_version_wraps", "negotiation", transportType="STREAMABLE_HTTP",
         currentVersion="1999-01-01")
scenario("negotiate_sse_pinned", "negotiation", transportType="SSE",
         currentVersion="2025-11-25")


# ---- unsupported-version classification ----------------------------------------------------------------------
scenario("unsupported_single", "unsupportedVersion", messages=["Unsupported MCP protocol version: 1.0"])
scenario("unsupported_causal_chain", "unsupportedVersion",
         messages=["outer io failure", "protocol version is not supported"])
scenario("unsupported_case_insensitive", "unsupportedVersion", messages=["UNSUPPORTED PROTOCOL VERSION"])
scenario("unsupported_unrelated", "unsupportedVersion", messages=["timeout", "connection reset"])
scenario("unsupported_no_messages", "unsupportedVersion", messages=[])


# ---- initialize result validation ---------------------------------------------------------------------------
scenario("init_supported", "initialize", currentVersion="2025-11-25",
         result={"protocolVersion": "2025-06-18"})
scenario("init_same_version", "initialize", currentVersion="2025-06-18",
         result={"protocolVersion": "2025-06-18"})
scenario("init_missing_version", "initialize", result={"capabilities": {}})
scenario("init_version_not_primitive", "initialize", result={"protocolVersion": 20250618})
scenario("init_unsupported_version", "initialize", result={"protocolVersion": "1999-01-01"})


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