cmake_minimum_required(VERSION 3.22)
project(agora_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Standalone (this directory used as the root project): build and test on the host. Embedded
# (add_subdirectory from the Android app): the tests need host fixtures and stay out of the APK.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(AGORA_CORE_TESTS_DEFAULT ON)
else()
    set(AGORA_CORE_TESTS_DEFAULT OFF)
endif()
option(AGORA_CORE_BUILD_TESTS "Build the agora core tests" ${AGORA_CORE_TESTS_DEFAULT})

add_library(agora_core_lib STATIC
    src/anthropic_request.cpp
    src/anthropic_stream.cpp
    src/bpe_vocabulary.cpp
    src/citations.cpp
    src/claude_chat_importer.cpp
    src/cjk_text.cpp
    src/task_schedule.cpp
    src/cron_expression.cpp
    src/custom_model_references.cpp
    src/backup_media_policy.cpp
    src/attachment_clone_session.cpp
    src/semantic_index_ledger.cpp
    src/maintenance_debt.cpp
    src/runtime_recovery.cpp
    src/context_compact.cpp
    src/chat_search_dao.cpp
    src/chat_media_references_dao.cpp
    src/chat_provider_context_dao.cpp
    src/new_chat_persist_dao.cpp
    src/chat_automation_dao.cpp
    src/chat_dao.cpp
    src/conversation_export_snapshot_reader.cpp
    src/message_persistence_guard.cpp
    src/imported_citation_projection.cpp
    src/native_backup_secrets.cpp
    src/native_backup_format.cpp
    src/native_run_archive.cpp
    src/export_extra_settings.cpp
    src/conch_sse.cpp
    src/tz.cpp
    src/duckduckgo.cpp
    src/export_spool.cpp
    src/embedding.cpp
    src/embedding_selector.cpp
    src/gemini_request.cpp
    src/gemini_stream.cpp
    src/java_uri.cpp
    src/mcp_protocol.cpp
    src/memory_manager.cpp
    src/skill_manager.cpp
    src/json.cpp
    src/loop_policy.cpp
    src/message_preparation.cpp
    src/model_aliases.cpp
    src/malformed_tool_call.cpp
    src/message_preparation_tokens.cpp
    src/ollama.cpp
    src/openai_request.cpp
    src/openai_request_json.cpp
    src/openai_responses_request.cpp
    src/openai_stream.cpp
    src/openai_stream_codec.cpp
    src/portable_glob.cpp
    src/portable_settings_reset.cpp
    src/run_ui_projection.cpp
    src/portable_settings_archive.cpp
    src/data_exporter.cpp
    src/data_importer.cpp
    src/android_json.cpp
    src/native_conversation_graph_source.cpp
    src/native_conversation_graph_importer.cpp
    src/run_branch_selection.cpp
    src/run_recovery.cpp
    src/conversation_runtime.cpp
    src/thinking_capability.cpp
    src/model_thinking_resolver.cpp
    src/provider_protocol_support.cpp
    src/token_pricing.cpp
    src/streaming_json.cpp
    src/openai_service_tiers.cpp
    src/context_budget.cpp
    src/message_source.cpp
    src/developer_mode_unlock.cpp
    src/database_compatibility.cpp
    src/v18_regeneration_repair.cpp
    src/regeneration_tree_repair.cpp
    src/legacy_run_backfill.cpp
    src/migration_16_17.cpp
    src/migration_17_18.cpp
    src/migration_18_19.cpp
    src/migration_19_20.cpp
    src/migration_20_35.cpp
    src/sqlite_platform.cpp
    src/uuid_v3.cpp
    src/provider_identity.cpp
    src/provider_support.cpp
    src/sha256.cpp
    src/shell_tool_definitions.cpp
    src/shell_tools.cpp
    src/shell_crypto.cpp
    src/conch_handshake.cpp
    src/shell_validation.cpp
    src/crypto_primitives.cpp
    src/bounded_wire_read.cpp
    src/conch_network.cpp
    src/http_policy.cpp
    src/shell_file_edit.cpp
    src/sandbox_virtual_paths.cpp
    src/sandbox_path_resolver.cpp
    src/shell_durable_jobs.cpp
    src/shell_client.cpp
    src/shell_tool_backends.cpp
    src/shell_tool_provider.cpp
    src/web_search_tool_provider.cpp
    src/sandbox_executor.cpp
    src/sandbox_manager.cpp
    src/alpine_package_index.cpp
    src/alpine_package_metadata_store.cpp
    src/stream_normalization.cpp
    src/stream_normalization_normalizer.cpp
    src/stream_normalization_text_tools.cpp
    src/stream_normalization_thinking.cpp
    src/text_counter.cpp
    src/token_count.cpp
    src/unicode.cpp
    src/zip_reader.cpp
    src/zip_writer.cpp
    src/native_backup_v5.cpp
    src/native_backup_archive.cpp
    src/gpt_chat_importer.cpp
    src/generation_policies.cpp
    src/generation_streaming_segments.cpp
)
target_include_directories(agora_core_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# The library gets linked into shared objects (the Android JNI shim, host smoke builds).
set_target_properties(agora_core_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(agora_core_lib PRIVATE -Wall -Wextra)
endif()

# Optional host-side SSH transport: the JSch surface SshClient.kt drives, backed by libssh2.
# Android ships without libssh2 (JSch stays the Kotlin stack there), so the APK is untouched.
find_path(AGORA_LIBSSH2_INCLUDE_DIR NAMES libssh2.h)
find_library(AGORA_LIBSSH2_LIBRARY NAMES ssh2 libssh2)
if(AGORA_LIBSSH2_INCLUDE_DIR AND AGORA_LIBSSH2_LIBRARY)
    set(AGORA_CORE_HAVE_LIBSSH2 ON)
    target_sources(agora_core_lib PRIVATE src/ssh_libssh2_transport.cpp)
    target_compile_definitions(agora_core_lib PUBLIC AGORA_CORE_HAVE_LIBSSH2=1)
    target_include_directories(agora_core_lib PRIVATE ${AGORA_LIBSSH2_INCLUDE_DIR})
    target_link_libraries(agora_core_lib PUBLIC ${AGORA_LIBSSH2_LIBRARY})
    message(STATUS "agora core: libssh2 SSH transport enabled (${AGORA_LIBSSH2_LIBRARY})")
else()
    set(AGORA_CORE_HAVE_LIBSSH2 OFF)
endif()

# Optional host-side HTTP transport: the OkHttp surface HttpClient.kt drives, backed by libcurl+OpenSSL.
# Android ships without libcurl (OkHttp stays the Kotlin stack there), so the APK is untouched.
find_package(CURL REQUIRED)
if(CURL_FOUND OR CURL_LIBRARY_RELEASE)
    set(AGORA_CORE_HAVE_CURL ON)
    target_sources(agora_core_lib PRIVATE src/curl_transport.cpp)
    target_compile_definitions(agora_core_lib PUBLIC AGORA_CORE_HAVE_CURL=1)
    target_include_directories(agora_core_lib PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(agora_core_lib PUBLIC ${CURL_LIBRARIES})
    message(STATUS "agora core: libcurl HTTP transport enabled (${CURL_LIBRARIES})")
else()
    set(AGORA_CORE_HAVE_CURL OFF)
endif()

if(AGORA_CORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)

    # Parity stand tool: mirrors core/tools/golden/GoldenDump.kt for the differentiation loop.
    add_executable(agora_core_dump tools/dump_pieces.cpp)
    target_link_libraries(agora_core_dump PRIVATE agora_core_lib)
endif()
