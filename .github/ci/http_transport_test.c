#define BITSXL_TEST 1
#define main bitsxl_program_main
#include "../../src/bitsxl.c"
#undef main

#include <assert.h>
#include <glob.h>

static int legacy_body_file_count(void) {
	glob_t matches = {0};
	int result = glob("/tmp/bitsxl_body_*", 0, NULL, &matches);
	int count = result == 0 ? (int)matches.gl_pathc : 0;
	assert(result == 0 || result == GLOB_NOMATCH);
	globfree(&matches);
	return count;
}

static char *fixture_fetch(const char *base, const char *path, const char *method, StrList *headers, const char *body) {
	char *url = xasprintf("%s%s", base, path);
	char *response = http_fetch(url, method, headers, body);
	free(url);
	return response;
}

static void test_get_and_redirect(const char *base) {
	StrList headers = {0};
	sl_add(&headers, hdr("X-Fixture: get-header"));
	sl_add(&headers, hdr("Host: fixture.example"));
	sl_add(&headers, hdr("Accept-Encoding: identity"));
	char *response = fixture_fetch(base, "/get", "GET", &headers, NULL);
	assert(!strcmp(response, "GET_OK"));
	free(response);
	free_sl(&headers);

	headers = (StrList){0};
	sl_add(&headers, hdr("X-Fixture: redirect-header"));
	sl_add(&headers, hdr("Host: fixture.example"));
	sl_add(&headers, hdr("Accept-Encoding: identity"));
	response = fixture_fetch(base, "/redirect", "GET", &headers, NULL);
	assert(!strcmp(response, "REDIRECT_OK"));
	free(response);
	free_sl(&headers);
}

static void test_post_bodies(const char *base) {
	const char *json = "{\"fixture\":\"post\",\"value\":\"alpha\"}";
	const char *form = "grant_type=refresh_token&refresh_token=a%2Bb";
	int before = legacy_body_file_count();
	StrList headers = {0};
	sl_add(&headers, hdr("Content-Type: application/json"));
	sl_add(&headers, hdr("X-Fixture: post-json"));
	char *response = fixture_fetch(base, "/post-json", "POST", &headers, json);
	assert(!strcmp(response, "POST_JSON_OK"));
	free(response);
	free_sl(&headers);

	headers = (StrList){0};
	sl_add(&headers, hdr("Content-Type: application/x-www-form-urlencoded"));
	sl_add(&headers, hdr("X-Fixture: post-form"));
	response = fixture_fetch(base, "/post-form", "POST", &headers, form);
	assert(!strcmp(response, "POST_FORM_OK"));
	free(response);
	free_sl(&headers);
	assert(legacy_body_file_count() == before);
}

static void test_http_error_bodies(const char *base) {
	char *response = fixture_fetch(base, "/status/404", "GET", NULL, NULL);
	assert(!strcmp(response, "STATUS_404"));
	free(response);
	response = fixture_fetch(base, "/status/500", "GET", NULL, NULL);
	assert(!strcmp(response, "STATUS_500"));
	free(response);
}

static void test_transport_failure_and_limit(const char *base) {
	char *response = fixture_fetch(base, "/close", "GET", NULL, NULL);
	assert(strstr(response, "\"status\":\"FAILED\"") != NULL);
	assert(strstr(response, "\"code\":\"HTTP_FETCH_FAILED\"") != NULL);
	free(response);

	response = fixture_fetch(base, "/large", "GET", NULL, NULL);
	char *expected = xasprintf("{\"status\":\"FAILED\",\"code\":\"HTTP_RESPONSE_TOO_LARGE\",\"limit\":%u}", HTTP_RESPONSE_MAX);
	assert(!strcmp(response, expected));
	free(expected);
	free(response);
}

int main(int argc, char **argv) {
	assert(argc == 2);
	test_get_and_redirect(argv[1]);
	test_post_bodies(argv[1]);
	test_http_error_bodies(argv[1]);
	test_transport_failure_and_limit(argv[1]);
	puts("http transport tests passed");
	return 0;
}
