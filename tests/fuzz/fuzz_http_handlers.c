/* libFuzzer harness: every handler of web.c without the Digest login
 * (WEB_TEST_NOAUTH, the host-test switch in handle_conn), so the fuzzer
 * reaches all /api handlers with arbitrary bodies. Same input format,
 * skipped paths and no-op sleep/sync as fuzz_http_request.c. */
#define WEB_TEST_NOAUTH 1
#include "fuzz_http_request.c"
