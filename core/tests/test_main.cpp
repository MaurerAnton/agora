#include "check.hpp"

namespace agora::core::test {

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

Registrar::Registrar(std::string name, std::function<void()> run) {
    registry().push_back(Case{std::move(name), std::move(run)});
}

void fail(const char* file, int line, const std::string& message) {
    throw Failure(std::string(file) + ":" + std::to_string(line) + ": " + message);
}

}  // namespace agora::core::test

int main() {
    int failed = 0;
    for (const agora::core::test::Case& test_case : agora::core::test::registry()) {
        try {
            test_case.run();
            std::printf("[ ok ] %s\n", test_case.name.c_str());
        } catch (const agora::core::test::Failure& failure) {
            failed++;
            std::printf("[FAIL] %s\n       %s\n", test_case.name.c_str(), failure.what());
        } catch (const std::exception& error) {
            failed++;
            std::printf("[FAIL] %s\n       unexpected: %s\n", test_case.name.c_str(), error.what());
        }
    }
    std::printf("\n%zu test(s), %d failure(s)\n", agora::core::test::registry().size(), failed);
    return failed == 0 ? 0 : 1;
}
