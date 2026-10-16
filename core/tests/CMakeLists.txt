find_package(Threads REQUIRED)

add_executable(agora_core_tests
    test_main.cpp
    token_count_test.cpp
    openai_stream_parity_test.cpp
    openai_stream_codec_test.cpp
    openai_request_parity_test.cpp
    openai_request_test.cpp
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
)

add_test(NAME agora_core_tests COMMAND agora_core_tests)
