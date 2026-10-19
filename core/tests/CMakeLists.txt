find_package(Threads REQUIRED)

add_executable(agora_core_tests
    test_main.cpp
    token_count_test.cpp
    openai_stream_parity_test.cpp
    openai_stream_codec_test.cpp
    openai_request_parity_test.cpp
    openai_request_test.cpp
    message_preparation_parity_test.cpp
    message_preparation_test.cpp
    stream_normalization_parity_test.cpp
    stream_normalization_test.cpp
    backup_media_policy_parity_test.cpp
    citations_parity_test.cpp
    cjk_text_parity_test.cpp
    task_schedule_parity_test.cpp
    cron_expression_parity_test.cpp
    conch_sse_parity_test.cpp
    anthropic_stream_parity_test.cpp
    anthropic_request_parity_test.cpp
    gemini_request_parity_test.cpp
    gemini_stream_parity_test.cpp
    ollama_parity_test.cpp
    portable_glob_parity_test.cpp
    model_aliases_parity_test.cpp
    custom_model_references_parity_test.cpp
    run_branch_selection_parity_test.cpp
    conversation_runtime_parity_test.cpp
    thinking_capability_parity_test.cpp
    token_pricing_parity_test.cpp
    streaming_json_parity_test.cpp
    provider_identity_parity_test.cpp
    provider_names_parity_test.cpp
    provider_support_parity_test.cpp
    duckduckgo_parity_test.cpp
    embedding_parity_test.cpp
    embedding_selector_parity_test.cpp
    loop_policy_parity_test.cpp
    mcp_protocol_parity_test.cpp
    memory_manager_parity_test.cpp
    shell_definitions_parity_test.cpp
    shell_tools_parity_test.cpp
    parity_test.cpp
    fuzz_parity_test.cpp
)
target_link_libraries(agora_core_tests PRIVATE agora_core_lib Threads::Threads)
target_include_directories(agora_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Fixtures: the shipped vocabulary and the tiktoken reference set live in the app tree, two
# levels up, so the port is always checked against exactly the files the app ships. The frozen
# corpus and its golden counts live with the golden tools.
target_compile_definitions(agora_core_tests PRIVATE
    AGORA_FIXTURE_VOCABULARY="${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/assets/tokenizers/o200k_base.tiktoken"
    AGORA_FIXTURE_REFERENCE="${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/test/resources/tokenizers/o200k_ref.json"
    AGORA_FIXTURE_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/corpus.b64"
    AGORA_FIXTURE_CORPUS_COUNTS="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/golden_counts.txt"
    AGORA_FIXTURE_OPENAI_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/openai"
    AGORA_FIXTURE_OPENAI_CODEC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/test/resources/openai_stream_codec"
    AGORA_FIXTURE_OPENAI_REQUEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/openai-request"
    AGORA_FIXTURE_MESSAGE_PREP_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/message-preparation"
    AGORA_FIXTURE_STREAM_NORMALIZATION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/stream-normalization"
    AGORA_FIXTURE_CITATIONS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/citations"
    AGORA_FIXTURE_CJK_TEXT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/cjk-text"
    AGORA_FIXTURE_ANTHROPIC_STREAM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/anthropic-stream"
    AGORA_FIXTURE_ANTHROPIC_REQUEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/anthropic-request"
    AGORA_FIXTURE_GEMINI_REQUEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/gemini-request"
    AGORA_FIXTURE_GEMINI_STREAM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/gemini-stream"
    AGORA_FIXTURE_OLLAMA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/ollama"
    AGORA_FIXTURE_GLOB_MATCHER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/glob-matcher"
    AGORA_FIXTURE_CUSTOM_MODEL_REFERENCES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/custom-model-references"
    AGORA_FIXTURE_RUN_BRANCH_SELECTION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/run-branch-selection"
    AGORA_FIXTURE_CONVERSATION_RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/conversation-runtime"
    AGORA_FIXTURE_THINKING_CAPABILITY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/thinking-capability"
    AGORA_FIXTURE_TOKEN_PRICING_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/token-pricing"
    AGORA_FIXTURE_STREAMING_JSON_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/streaming-json-document"
    AGORA_FIXTURE_LOOP_POLICY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/loop-policy"
    AGORA_FIXTURE_MODEL_ALIASES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/model-aliases"
    AGORA_FIXTURE_PROVIDER_NAMES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/provider-names"
    AGORA_FIXTURE_PROVIDER_IDENTITY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/provider-identity"
    AGORA_FIXTURE_PROVIDER_SUPPORT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/provider-support"
    AGORA_FIXTURE_DUCKDUCKGO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/duckduckgo"
    AGORA_FIXTURE_MCP_PROTOCOL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/mcp-protocol"
    AGORA_FIXTURE_EMBEDDING_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/embedding"
    AGORA_FIXTURE_SHELL_DEFINITIONS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/shell-definitions"
    AGORA_FIXTURE_SHELL_TOOLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/shell-tools"
    AGORA_FIXTURE_MEMORY_MANAGER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/memory-manager"
    AGORA_FIXTURE_EMBEDDING_SELECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/embedding-selector"
    AGORA_FIXTURE_CONCH_SSE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/conch-sse"
    AGORA_FIXTURE_CRON_EXPRESSION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/cron-expression"
    AGORA_FIXTURE_TASK_SCHEDULE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/task-schedule"
    AGORA_FIXTURE_BACKUP_MEDIA_POLICY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/golden/backup-media-policy"
    AGORA_FIXTURE_PREP_VOCAB="${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/assets/tokenizers/o200k_base.tiktoken"
)

add_test(NAME agora_core_tests COMMAND agora_core_tests)
