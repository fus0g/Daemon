// SPDX-License-Identifier: GPL-3.0-or-later
// Test runner: executes every group and reports the failure count.
import std;

int failures = 0;

void runBaseTests();
void runTextTests();
void runServicesTests();
void runUiTests();
void testMarkdown();
void testMarkdownLayout();
void testReadOnlyBuffer();
void testImages();

int main() {
    runBaseTests();
    runTextTests();
    testMarkdown();
    testMarkdownLayout();
    testReadOnlyBuffer();
    testImages();
    runServicesTests();
    runUiTests();
    if (failures == 0) std::println("all tests passed");
    return failures == 0 ? 0 : 1;
}
