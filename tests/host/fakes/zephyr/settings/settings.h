/* Host-test fake for <zephyr/settings/settings.h>.
 *
 * settings_name_steq() has REAL path-segment-matching semantics below
 * (not a no-op) since settings_store.c's haven_settings_set() dispatch
 * logic -- the actual thing under test -- depends on it working correctly.
 * Everything else here is a link-satisfying stub.
 */
#ifndef FAKE_ZEPHYR_SETTINGS_SETTINGS_H_
#define FAKE_ZEPHYR_SETTINGS_SETTINGS_H_

#include <stddef.h>
#include <string.h>
#include <sys/types.h>

typedef ssize_t (*settings_read_cb)(void *cb_arg, void *data, size_t len);

/* Real Zephyr semantics: true if `name`'s first path segment (up to '/' or
 * end of string) equals `key`. On match, `*next` is set to the remainder
 * after the separator, or NULL if `name` == `key` exactly (no more
 * segments) -- which is exactly the `&& !next` check haven_settings_set()
 * relies on to mean "this is the leaf, not a deeper subtree".
 */
static inline int settings_name_steq(const char *name, const char *key, const char **next)
{
	size_t klen = strlen(key);

	if (strncmp(name, key, klen) != 0) {
		return 0;
	}
	if (name[klen] == '\0') {
		if (next) {
			*next = NULL;
		}
		return 1;
	}
	if (name[klen] == '/') {
		if (next) {
			*next = &name[klen + 1];
		}
		return 1;
	}
	return 0;
}

static inline int settings_save_one(const char *key, const void *value, size_t val_len)
{
	(void)key;
	(void)value;
	(void)val_len;
	return 0;
}

static inline int settings_subsys_init(void)
{
	return 0;
}

static inline int settings_load_subtree(const char *subtree)
{
	(void)subtree;
	return 0;
}

/* No-op registration -- the test calls haven_settings_set() directly
 * rather than going through Zephyr's real settings-handler iteration, so
 * nothing needs to actually happen here, just compile.
 */
#define SETTINGS_STATIC_HANDLER_DEFINE(_hname, _tree, _get, _set, _export, _commit) \
	struct haven_test_unused_settings_handler_marker_##_hname { int unused; }

#endif /* FAKE_ZEPHYR_SETTINGS_SETTINGS_H_ */
