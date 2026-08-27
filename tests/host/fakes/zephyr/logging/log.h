/* Host-test fake for <zephyr/logging/log.h> -- logging is irrelevant to the
 * logic under test, so every macro is a no-op (LOG_MODULE_REGISTER still
 * needs to consume its arguments syntactically so the production file's
 * top-level invocation compiles).
 */
#ifndef FAKE_ZEPHYR_LOGGING_LOG_H_
#define FAKE_ZEPHYR_LOGGING_LOG_H_

#define LOG_LEVEL_INF 0
#define LOG_LEVEL_DBG 0

#define LOG_MODULE_REGISTER(name, level) static const int haven_test_unused_log_module_##name = (level)

#define LOG_INF(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)

#endif /* FAKE_ZEPHYR_LOGGING_LOG_H_ */
