#pragma once

// Each test suite is a free function; the runner in test_main.cpp invokes them.
void suite_paths();
void suite_store();
void suite_edit_tools();
void suite_policy();
void suite_revert_e2e();
void suite_partial_reply();
void suite_highlight();
void suite_repomap();
void suite_llm_cache();
void suite_llm_body();
void suite_systemprompt();
void suite_autoverify();
void suite_shell();
void suite_image();
void suite_debugrunner();
