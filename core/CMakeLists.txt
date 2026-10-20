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
    src/cjk_text.cpp
    src/task_schedule.cpp
    src/cron_expression.cpp
    src/custom_model_references.cpp
    src/backup_media_policy.cpp
    src/conch_sse.cpp
    src/tz.cpp
    src/duckduckgo.cpp
    src/embedding.cpp
    src/embedding_selector.cpp
    src/gemini_request.cpp
    src/gemini_stream.cpp
    src/java_uri.cpp
    src/mcp_protocol.cpp
    src/memory_manager.cpp
    src/json.cpp
    src/loop_policy.cpp
    src/message_preparation.cpp
    src/model_aliases.cpp
    src/message_preparation_tokens.cpp
    src/ollama.cpp
    src/openai_request.cpp
    src/openai_request_json.cpp
    src/openai_stream.cpp
    src/openai_stream_codec.cpp
    src/portable_glob.cpp
    src/run_branch_selection.cpp
    src/conversation_runtime.cpp
    src/thinking_capability.cpp
    src/token_pricing.cpp
    src/streaming_json.cpp
    src/openai_service_tiers.cpp
    src/context_budget.cpp
    src/message_source.cpp
    src/provider_identity.cpp
    src/provider_support.cpp
    src/sha256.cpp
    src/shell_tool_definitions.cpp
    src/shell_tools.cpp
    src/stream_normalization.cpp
    src/stream_normalization_normalizer.cpp
    src/stream_normalization_text_tools.cpp
    src/stream_normalization_thinking.cpp
    src/text_counter.cpp
    src/token_count.cpp
    src/unicode.cpp
)
target_include_directories(agora_core_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# The library gets linked into shared objects (the Android JNI shim, host smoke builds).
set_target_properties(agora_core_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(agora_core_lib PRIVATE -Wall -Wextra)
endif()

if(AGORA_CORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)

    # Parity stand tool: mirrors core/tools/golden/GoldenDump.kt for the differentiation loop.
    add_executable(agora_core_dump tools/dump_pieces.cpp)
    target_link_libraries(agora_core_dump PRIVATE agora_core_lib)
endif()
