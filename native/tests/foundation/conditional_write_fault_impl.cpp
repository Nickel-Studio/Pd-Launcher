#include "core/common/cancellation.hpp"
#ifdef _WIN32
#include <windows.h>
#define WriteFile conditional_test_WriteFile
#define FlushFileBuffers conditional_test_FlushFileBuffers
#else
#include <unistd.h>
#define write conditional_test_write
#define fsync conditional_test_fsync
#endif

// Compile the actual replacement body with test-only boundary/OS failures.
// No production runtime hook or alternative transaction implementation exists.
#define valid_utf8 conditional_test_valid_utf8
#define path_from_utf8 conditional_test_path_from_utf8
#define path_to_utf8 conditional_test_path_to_utf8
#define reject_path_links conditional_test_reject_path_links
#define read_file_bounded conditional_test_read_file_bounded
#define read_text_bounded conditional_test_read_text_bounded
#define unique_token conditional_test_unique_token
#define create_file_exclusive conditional_test_create_file_exclusive
#define atomic_write_file conditional_test_atomic_write_file
#define atomic_write_text conditional_test_atomic_write_text
#define atomic_copy_no_replace conditional_test_atomic_copy_no_replace
#define rename_no_replace conditional_test_rename_no_replace
#define sync_directory conditional_test_sync_directory
#define FileLock ConditionalTestFileLock
#define atomic_create_file conditional_test_atomic_create_file
#define move_file_if_matches conditional_test_move_file_if_matches
#define remove_file_if_matches conditional_test_remove_file_if_matches
#define replace_file_conditionally foundation_test_replace_file_conditionally
#define replace_text_conditionally foundation_test_replace_text_conditionally
#define NICKEL_CONDITIONAL_TEST_REPLACE_ONLY
#define NICKEL_CONDITIONAL_TEST_HOOK(point, target, stage, backup, receipt) \
    foundation_test::conditional_hook(point, target, stage, backup, receipt)
#include "conditional_write_faults.hpp"
#include "../../core/config/atomic_file.cpp"
