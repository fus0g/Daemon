// Test runner: executes every group and reports the failure count.
import std;

int failures = 0;

void runBaseTests();
void runTextTests();
void runServicesTests();
void runUiTests();

int main() {
    runBaseTests();
    runTextTests();
    runServicesTests();
    runUiTests();
    if (failures == 0) std::println("all tests passed");
    return failures == 0 ? 0 : 1;
}
