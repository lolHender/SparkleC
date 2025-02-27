#include <stdio.h>

#include <curl/curl.h>
#include <jansson.h>

#if defined(_WIN32) && defined(_UNICODE)
	#include "wio.h"
#endif

#include "credentials.h"
#include "resources.h"
#include "cleanup.h"
#include "errors.h"
#include "query.h"
#include "callbacks.h"
#include "types.h"
#include "stringu.h"
#include "symbols.h"
#include "m3u8.h"
#include "vimeo.h"
#include "youtube.h"
#include "curl.h"
#include "hotmart.h"
#include "html.h"
#include "ttidy.h"

static const char HTTP_HEADER_AUTHORIZATION[] = "Authorization";
static const char HTTP_HEADER_REFERER[] = "Referer";
static const char HTTP_HEADER_TOKEN[] = "Token";
static const char HTTP_HEADER_CLUB[] = "Club";

static const char HTTP_AUTHENTICATION_BEARER[] = "Bearer";

#define HOTMART_API_CLUB_PREFIX "https://api-club.hotmart.com/hot-club-api/rest/v3"
#define HOTMART_API_SEC_VLC_PREFIX "https://api-sec-vlc.hotmart.com"
#define HOTMART_API_VLC_PREFIX "https://api-vlc.hotmart.com"
#define SPARKLEAPP_API_PREFIX "https://api.sparkleapp.com.br"

static const char HOTMART_HOMEPAGE[] = "https://hotmart.com";

static const char HOTMART_NAVIGATION_ENDPOINT[] = 
	HOTMART_API_CLUB_PREFIX
	"/navigation";

static const char HOTMART_MEMBERSHIP_ENDPOINT[] = 
	HOTMART_API_CLUB_PREFIX
	"/membership";

static const char HOTMART_PAGE_ENDPOINT[] = 
	HOTMART_API_CLUB_PREFIX
	"/page";

static const char HOTMART_ATTACHMENT_ENDPOINT[] = 
	HOTMART_API_CLUB_PREFIX
	"/attachment";

static const char HOTMART_TOKEN_ENDPOINT[] = 
	SPARKLEAPP_API_PREFIX
	"/oauth/token";

static const char HOTMART_TOKEN_CHECK_ENDPOINT[] = 
	HOTMART_API_SEC_VLC_PREFIX
	"/security/oauth/check_token";

static const char HOTMART_PROFILE_ENDPOINT[] = 
	HOTMART_API_VLC_PREFIX
	"/userprofile/rest/v1/user";

static const char VIMEO_URL_PATTERN[] = "https://player.vimeo.com/video";
static const char YOUTUBE_URL_PATTERN[] = "https://www.youtube.com/embed";

int hotmart_authorize(
	const char* const username,
	const char* const password,
	struct Credentials* const credentials
) {
	
	CURL* curl_easy = get_global_curl_easy();
	
	char* user __attribute__((__cleanup__(curlcharpp_free))) = curl_easy_escape(NULL, username, 0);
	
	if (user == NULL) {
		return UERR_CURL_FAILURE;
	}
	
	char* pass __attribute__((__cleanup__(curlcharpp_free))) = curl_easy_escape(NULL, password, 0);
	
	if (pass == NULL) {
		return UERR_CURL_FAILURE;
	}
	
	struct Query query __attribute__((__cleanup__(query_free))) = {0};
	
	add_parameter(&query, "grant_type", "password");
	add_parameter(&query, "username", user);
	add_parameter(&query, "password", pass);
	
	char* post_fields __attribute__((__cleanup__(charpp_free))) = NULL;
	const int code = query_stringify(query, &post_fields);
	
	if (code != UERR_SUCCESS) {
		return code;
	}
	
	struct String string __attribute__((__cleanup__(string_free))) = {0};
	
	curl_easy_setopt(curl_easy, CURLOPT_COPYPOSTFIELDS, post_fields);
	curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, curl_write_string_cb);
	curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, &string);
	curl_easy_setopt(curl_easy, CURLOPT_URL, HOTMART_TOKEN_ENDPOINT);
	
	if (curl_easy_perform_retry(curl_easy) != CURLE_OK) {
		return UERR_CURL_FAILURE;
	}
	
	json_auto_t* tree = json_loads(string.s, 0, NULL);
	
	if (tree == NULL) {
		return UERR_JSON_CANNOT_PARSE;
	}
	
	const json_t* obj = json_object_get(tree, "access_token");
	
	if (obj == NULL) {
		return UERR_JSON_MISSING_REQUIRED_KEY;
	}
	
	if (!json_is_string(obj)) {
		return UERR_JSON_NON_MATCHING_TYPE;
	}
	
	const char* const access_token = json_string_value(obj);
	
	credentials->access_token = malloc(strlen(access_token) + 1);
	
	if (credentials->access_token == NULL) {
		return UERR_MEMORY_ALLOCATE_FAILURE;
	}
	
	strcpy(credentials->access_token, access_token);
	
	string_free(&string);
	
	char authorization[strlen(HTTP_AUTHENTICATION_BEARER) + strlen(SPACE) + strlen(credentials->access_token) + 1];
	strcpy(authorization, HTTP_AUTHENTICATION_BEARER);
	strcat(authorization, SPACE);
	strcat(authorization, credentials->access_token);
	
	const char* const headers[][2] = {
		{HTTP_HEADER_AUTHORIZATION, authorization}
	};
	
	struct curl_slist* list __attribute__((__cleanup__(curl_slistp_free_all))) = NULL;
	
	for (size_t index = 0; index < sizeof(headers) / sizeof(*headers); index++) {
		const char* const* const header = headers[index];
		
		const char* const key = header[0];
		const char* const value = header[1];
		
		char item[strlen(key) + strlen(HTTP_HEADER_SEPARATOR) + strlen(value) + 1];
		strcpy(item, key);
		strcat(item, HTTP_HEADER_SEPARATOR);
		strcat(item, value);
		
		struct curl_slist* tmp = curl_slist_append(list, item);
		
		if (tmp == NULL) {
			return UERR_CURL_FAILURE;
		}
		
		list = tmp;
	}
	
	curl_easy_setopt(curl_easy, CURLOPT_HTTPGET, 1L);
	curl_easy_setopt(curl_easy, CURLOPT_HTTPHEADER, list);
	curl_easy_setopt(curl_easy, CURLOPT_URL, HOTMART_PROFILE_ENDPOINT);
	
	if (curl_easy_perform_retry(curl_easy) != CURLE_OK) {
		return UERR_CURL_FAILURE;
	}
	
	json_auto_t* subtree = json_loads(string.s, 0, NULL);
	
	if (subtree == NULL) {
		return UERR_JSON_CANNOT_PARSE;
	}
	
	obj = json_object_get(subtree, "name");
	
	if (obj == NULL) {
		return UERR_JSON_MISSING_REQUIRED_KEY;
	}
	
	if (!json_is_string(obj)) {
		return UERR_JSON_NON_MATCHING_TYPE;
	}
	
	const char* const name = json_string_value(obj);
	
	credentials->username = malloc(strlen(name) + 1);
	
	if (credentials->username == NULL) {
		return UERR_MEMORY_ALLOCATE_FAILURE;
	}
	
	strcpy(credentials->username, name);
	
	curl_easy_setopt(curl_easy, CURLOPT_HTTPHEADER, NULL);
	curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, NULL);
	curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, NULL);
	curl_easy_setopt(curl_easy, CURLOPT_URL, NULL);
	
	return UERR_SUCCESS;
	
}

int hotmart_get_resources(
	const struct Credentials* const credentials,
	struct Resources* const resources
) {
	
	CURL* curl_easy = get_global_curl_easy();
	
	struct Query query __attribute__((__cleanup__(query_free))) = {0};
	
	add_parameter(&query, "token", credentials->access_token);
	
	char* squery __attribute__((__cleanup__(charpp_free))) = NULL;
	const int code = query_stringify(query, &squery);
	
	if (code != UERR_SUCCESS) {
		return code;
	}
	
	CURLU* cu __attribute__((__cleanup__(curlupp_free))) = curl_url();
	
	if (cu == NULL) {
		return UERR_CURLU_FAILURE;
	}
	
	if (curl_url_set(cu, CURLUPART_URL, HOTMART_TOKEN_CHECK_ENDPOINT, 0) != CURLUE_OK) {
		return UERR_CURLU_FAILURE;
	}
	
	if (curl_url_set(cu, CURLUPART_QUERY, squery, 0) != CURLUE_OK) {
		return UERR_CURLU_FAILURE;
	}
	
	char* url __attribute__((__cleanup__(curlcharpp_free))) = NULL;
	
	if (curl_url_get(cu, CURLUPART_URL, &url, 0) != CURLUE_OK) {
		return UERR_CURLU_FAILURE;
	}
	
	curl_easy_setopt(curl_easy, CURLOPT_URL, url);
	
	struct String string __attribute__((__cleanup__(string_free))) = {0};
	
	curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, curl_write_string_cb);
	curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, &string);
	curl_easy_setopt(curl_easy, CURLOPT_HTTPGET, 1L);
	
	switch (curl_easy_perform_retry(curl_easy)) {
		case CURLE_OK:
			break;
		case CURLE_HTTP_RETURNED_ERROR:
			return UERR_PROVIDER_SESSION_EXPIRED;
		default:
			return UERR_CURL_FAILURE;
	}
	
	json_auto_t* tree = json_loads(string.s, 0, NULL);
	
	if (tree == NULL) {
		return UERR_JSON_CANNOT_PARSE;
	}
	
	const json_t* obj = json_object_get(tree, "resources");
	
	if (obj == NULL) {
		return UERR_JSON_MISSING_REQUIRED_KEY;
	}
	
	if (!json_is_array(obj)) {
		return UERR_JSON_NON_MATCHING_TYPE;
	}
	
	char authorization[strlen(HTTP_AUTHENTICATION_BEARER) + strlen(SPACE) + strlen(credentials->access_token) + 1];
	strcpy(authorization, HTTP_AUTHENTICATION_BEARER);
	strcat(authorization, SPACE);
	strcat(authorization, credentials->access_token);
	
	size_t index = 0;
	const json_t* item = NULL;
	const size_t array_size = json_array_size(obj);
	
	curl_easy_setopt(curl_easy, CURLOPT_URL, HOTMART_MEMBERSHIP_ENDPOINT);
	
	resources->size = sizeof(struct Resource) * array_size;
	resources->items = malloc(resources->size);
	
	if (resources->items == NULL) {
		return UERR_MEMORY_ALLOCATE_FAILURE;
	}
	
	json_array_foreach(obj, index, item) {
		if (!json_is_object(item)) {
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		const json_t* obj = json_object_get(item, "resource");
		
		if (obj == NULL) {
			return UERR_JSON_MISSING_REQUIRED_KEY;
		}
		
		if (!json_is_object(obj)) {
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		obj = json_object_get(obj, "subdomain");
		
		if (obj == NULL) {
			return UERR_JSON_MISSING_REQUIRED_KEY;
		}
		
		if (!json_is_string(obj)) {
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		const char* const id = json_string_value(obj);
		
		const char* const headers[][2] = {
			{HTTP_HEADER_AUTHORIZATION, authorization},
			{HTTP_HEADER_CLUB, id}
		};
		
		struct curl_slist* list __attribute__((__cleanup__(curl_slistp_free_all))) = NULL;
		
		for (size_t index = 0; index < sizeof(headers) / sizeof(*headers); index++) {
			const char** const header = (const char**) headers[index];
			const char* const key = header[0];
			const char* const value = header[1];
			
			char item[strlen(key) + strlen(HTTP_HEADER_SEPARATOR) + strlen(value) + 1];
			strcpy(item, key);
			strcat(item, HTTP_HEADER_SEPARATOR);
			strcat(item, value);
			
			struct curl_slist* tmp = curl_slist_append(list, item);
			
			if (tmp == NULL) {
				return UERR_CURL_FAILURE;
			}
			
			list = tmp;
		}
		
		struct String string __attribute__((__cleanup__(string_free))) = {0};
		
		curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, &string);
		curl_easy_setopt(curl_easy, CURLOPT_HTTPHEADER, list);
		
		if (curl_easy_perform_retry(curl_easy) != CURLE_OK) {
			return UERR_CURL_FAILURE;
		}
		
		json_auto_t* subtree = json_loads(string.s, 0, NULL);
		
		if (subtree == NULL) {
			return UERR_JSON_CANNOT_PARSE;
		}
		
		obj = json_object_get(subtree, "name");
		
		if (obj == NULL) {
			return UERR_JSON_MISSING_REQUIRED_KEY;
		}
		
		if (!json_is_string(obj)) {
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		const char* const name = json_string_value(obj);
		
		struct Resource resource = {
			.id = malloc(strlen(id) + 1),
			.name = malloc(strlen(name) + 1),
			.dirname = malloc(strlen(name) + 1),
			.short_dirname = malloc(strlen(id) + 1),
			.url = malloc(strlen(HTTPS_SCHEME) + strlen(id) + strlen(HOTMART_CLUB_SUFFIX) + 1)
		};
		
		if (resource.id == NULL || resource.name == NULL || resource.dirname == NULL || resource.short_dirname == NULL || resource.url == NULL) {
			return UERR_MEMORY_ALLOCATE_FAILURE;
		}
		
		strcpy(resource.id, id);
		strcpy(resource.name, name);
		
		strcpy(resource.dirname, name);
		normalize_directory(resource.dirname);
		
		strcpy(resource.short_dirname, id);
		
		strcpy(resource.url, HTTPS_SCHEME);
		strcat(resource.url, id);
		strcat(resource.url, HOTMART_CLUB_SUFFIX);
		
		resources->items[resources->offset++] = resource;
	}
	
	curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, NULL);
	curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, NULL);
	curl_easy_setopt(curl_easy, CURLOPT_HTTPHEADER, NULL);
	curl_easy_setopt(curl_easy, CURLOPT_URL, NULL);
	
	return UERR_SUCCESS;
	
}

int hotmart_get_modules(
	const struct Credentials* const credentials,
	struct Resource* const resource
) {
	
	CURL* curl_easy = get_global_curl_easy();
	
	char authorization[strlen(HTTP_AUTHENTICATION_BEARER) + strlen(SPACE) + strlen(credentials->access_token) + 1];
	strcpy(authorization, HTTP_AUTHENTICATION_BEARER);
	strcat(authorization, SPACE);
	strcat(authorization, credentials->access_token);
	
	const char* const headers[][2] = {
		{HTTP_HEADER_AUTHORIZATION, authorization},
		{HTTP_HEADER_CLUB, resource->id}
	};
	
	struct curl_slist* list __attribute__((__cleanup__(curl_slistp_free_all))) = NULL;
	
	for (size_t index = 0; index < sizeof(headers) / sizeof(*headers); index++) {
		const char* const* const header = headers[index];
		
		const char* const key = header[0];
		const char* const value = header[1];
		
		char item[strlen(key) + strlen(HTTP_HEADER_SEPARATOR) + strlen(value) + 1];
		strcpy(item, key);
		strcat(item, HTTP_HEADER_SEPARATOR);
		strcat(item, value);
		
		struct curl_slist* tmp = curl_slist_append(list, item);
		
		if (tmp == NULL) {
			return UERR_CURL_FAILURE;
		}
		
		list = tmp;
	}
	
	struct String string __attribute__((__cleanup__(string_free))) = {0};
	
	curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, curl_write_string_cb);
	curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, &string);
	curl_easy_setopt(curl_easy, CURLOPT_HTTPHEADER, list);
	curl_easy_setopt(curl_easy, CURLOPT_URL, HOTMART_NAVIGATION_ENDPOINT);
	
	if (curl_easy_perform_retry(curl_easy) != CURLE_OK) {
		return UERR_CURL_FAILURE;
	}
	
	json_auto_t* tree = json_loads(string.s, 0, NULL);
	
	if (tree == NULL) {
		return UERR_JSON_CANNOT_PARSE;
	}
	
	const json_t* obj = json_object_get(tree, "modules");
	
	if (obj == NULL) {
		return UERR_JSON_MISSING_REQUIRED_KEY;
	}
	
	if (!json_is_array(obj)) {
		return UERR_JSON_NON_MATCHING_TYPE;
	}
	
	size_t index = 0;
	const json_t* item = NULL;
	const size_t array_size = json_array_size(obj);
	
	curl_easy_setopt(curl_easy, CURLOPT_URL, HOTMART_MEMBERSHIP_ENDPOINT);
	
	resource->modules.size = sizeof(struct Module) * array_size;
	resource->modules.items = malloc(resource->modules.size);
	
	if (resource->modules.items == NULL) {
		return UERR_MEMORY_ALLOCATE_FAILURE;
	}
	
	json_array_foreach(obj, index, item) {
		if (!json_is_object(item)) {
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		const json_t* obj = json_object_get(item, "id");
		
		if (obj == NULL) {
			return UERR_JSON_MISSING_REQUIRED_KEY;
		}
		
		if (!json_is_string(obj)) {
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		const char* const id = json_string_value(obj);
		
		obj = json_object_get(item, "name");
		
		if (obj == NULL) {
			return UERR_JSON_MISSING_REQUIRED_KEY;
		}
		
		if (!json_is_string(obj)) {
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		const char* const name = json_string_value(obj);
		
		obj = json_object_get(item, "locked");
		
		if (obj == NULL) {
			return UERR_JSON_MISSING_REQUIRED_KEY;
		}
		
		if (!json_is_boolean(obj)) {
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		const int is_locked = json_boolean_value(obj);
		
		struct Module module = {
			.id = malloc(strlen(id) + 1),
			.name = malloc(strlen(name) + 1),
			.dirname = malloc(strlen(name) + 1),
			.short_dirname = malloc(strlen(id) + 1),
			.is_locked = is_locked
		};
		
		if (module.id == NULL || module.name == NULL || module.dirname == NULL || module.short_dirname == NULL) {
			return UERR_MEMORY_ALLOCATE_FAILURE;
		}
		
		strcpy(module.id, id);
		strcpy(module.name, name);
		
		strcpy(module.dirname, name);
		normalize_directory(module.dirname);
		
		strcpy(module.short_dirname, id);
		
		obj = json_object_get(item, "pages");
		
		if (obj == NULL) {
			return UERR_JSON_MISSING_REQUIRED_KEY;
		}
		
		if (!json_is_array(obj)) {
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		size_t page_index = 0;
		json_t* page_item = NULL;
		const size_t array_size = json_array_size(obj);
		
		module.pages.size = sizeof(struct Page) * array_size;
		module.pages.items = malloc(module.pages.size);
		
		if (module.pages.items == NULL) {
			return UERR_MEMORY_ALLOCATE_FAILURE;
		}
		
		json_array_foreach(obj, page_index, page_item) {
			if (!json_is_object(page_item)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const json_t* obj = json_object_get(page_item, "hash");
			
			if (obj == NULL) {
				return UERR_JSON_MISSING_REQUIRED_KEY;
			}
			
			if (!json_is_string(obj)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const char* const id = json_string_value(obj);
			
			obj = json_object_get(page_item, "name");
			
			if (obj == NULL) {
				return UERR_JSON_MISSING_REQUIRED_KEY;
			}
			
			if (!json_is_string(obj)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const char* const name = json_string_value(obj);
			
			obj = json_object_get(page_item, "locked");
			
			if (obj == NULL) {
				return UERR_JSON_MISSING_REQUIRED_KEY;
			}
			
			if (!json_is_boolean(obj)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const int is_locked = json_boolean_value(obj);
			
			struct Page page = {
				.id = malloc(strlen(id) + 1),
				.name = malloc(strlen(name) + 1),
				.dirname = malloc(strlen(name) + 1),
				.short_dirname = malloc(strlen(id) + 1),
				.is_locked = is_locked
			};
			
			if (page.id == NULL || page.name == NULL || page.dirname == NULL || page.short_dirname == NULL) {
				return UERR_MEMORY_ALLOCATE_FAILURE;
			}
			
			strcpy(page.id, id);
			strcpy(page.name, name);
			
			strcpy(page.dirname, name);
			normalize_directory(page.dirname);
			
			strcpy(page.short_dirname, id);
			
			module.pages.items[module.pages.offset++] = page;
		}
		
		resource->modules.items[resource->modules.offset++] = module;
	}
	
	curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, NULL);
	curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, NULL);
	curl_easy_setopt(curl_easy, CURLOPT_HTTPHEADER, NULL);
	curl_easy_setopt(curl_easy, CURLOPT_URL, NULL);
	
	return UERR_SUCCESS;
	
}

int hotmart_get_module(
	const struct Credentials* const credentials,
	const struct Resource* const resource,
	struct Module* const module
) {
	
	(void) credentials;
	(void) resource;
	(void) module;
	
	return UERR_NOT_IMPLEMENTED;
	
}

int hotmart_get_page(
	const struct Credentials* const credentials,
	const struct Resource* const resource,
	struct Page* const page
) {
	
	CURL* curl_easy = get_global_curl_easy();
	
	char authorization[strlen(HTTP_AUTHENTICATION_BEARER) + strlen(SPACE) + strlen(credentials->access_token) + 1];
	strcpy(authorization, HTTP_AUTHENTICATION_BEARER);
	strcat(authorization, SPACE);
	strcat(authorization, credentials->access_token);
	
	const char* const headers[][2] = {
		{HTTP_HEADER_AUTHORIZATION, authorization},
		{HTTP_HEADER_CLUB, resource->id},
		{HTTP_HEADER_REFERER, HOTMART_HOMEPAGE}
	};
	
	struct curl_slist* list __attribute__((__cleanup__(curl_slistp_free_all))) = NULL;
	
	for (size_t index = 0; index < sizeof(headers) / sizeof(*headers); index++) {
		const char* const* const header = headers[index];
		
		const char* const key = header[0];
		const char* const value = header[1];
		
		char item[strlen(key) + strlen(HTTP_HEADER_SEPARATOR) + strlen(value) + 1];
		strcpy(item, key);
		strcat(item, HTTP_HEADER_SEPARATOR);
		strcat(item, value);
		
		struct curl_slist* tmp = curl_slist_append(list, item);
		
		if (tmp == NULL) {
			return UERR_CURL_FAILURE;
		}
		
		list = tmp;
	}
	
	struct String string __attribute__((__cleanup__(string_free))) = {0};
	
	curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, curl_write_string_cb);
	curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, &string);
	curl_easy_setopt(curl_easy, CURLOPT_HTTPHEADER, list);
	
	char url[strlen(HOTMART_PAGE_ENDPOINT) + strlen(SLASH) + strlen(page->id) + 1];
	strcpy(url, HOTMART_PAGE_ENDPOINT);
	strcat(url, SLASH);
	strcat(url, page->id);
	
	curl_easy_setopt(curl_easy, CURLOPT_URL, url);
	
	if (curl_easy_perform_retry(curl_easy) != CURLE_OK) {
		return UERR_CURL_FAILURE;
	}
	
	json_auto_t* tree = json_loads(string.s, 0, NULL);
	
	if (tree == NULL) {
		return UERR_JSON_CANNOT_PARSE;
	}
	
	const json_t* obj = json_object_get(tree, "mediasSrc");
	
	if (obj != NULL) {
		if (!json_is_array(obj)) {
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		size_t index = 0;
		const json_t* item = NULL;
		const size_t array_size = json_array_size(obj);
		
		page->medias.size = sizeof(struct Media) * array_size;
		page->medias.items = malloc(page->medias.size);
		
		if (page->medias.items == NULL) {
			return UERR_MEMORY_ALLOCATE_FAILURE;
		}
		
		json_array_foreach(obj, index, item) {
			if (!json_is_object(item)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const json_t* obj = json_object_get(item, "mediaSrcUrl");
			
			if (obj == NULL) {
				return UERR_JSON_MISSING_REQUIRED_KEY;
			}
			
			if (!json_is_string(obj)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const char* const media_page = json_string_value(obj);
			
			obj = json_object_get(item, "mediaName");
			
			if (obj == NULL) {
				return UERR_JSON_MISSING_REQUIRED_KEY;
			}
			
			if (!json_is_string(obj)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const char* const media_name = json_string_value(obj);
			
			obj = json_object_get(item, "mediaCode");
			
			if (obj == NULL) {
				return UERR_JSON_MISSING_REQUIRED_KEY;
			}
			
			if (!json_is_string(obj)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const char* const media_code = json_string_value(obj);
			
			obj = json_object_get(item, "mediaType");
			
			if (obj == NULL) {
				return UERR_JSON_MISSING_REQUIRED_KEY;
			}
			
			if (!json_is_string(obj)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const char* const media_type = json_string_value(obj);
			
			struct String string __attribute__((__cleanup__(string_free))) = {0};
			
			curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, &string);
			curl_easy_setopt(curl_easy, CURLOPT_URL, media_page);
			
			if (curl_easy_perform_retry(curl_easy) != CURLE_OK) {
				return UERR_CURL_FAILURE;
			}
			
			const char* const ptr = strstr(string.s, "mediaAssets");
			
			if (ptr == NULL) {
				return UERR_STRSTR_FAILURE;
			}
			
			const char* const start = strstr(ptr, HTTPS_SCHEME);
			const char* const end = strstr(start, QUOTATION_MARK);
			
			size_t size = (size_t) (end - start);
			
			char url[size + 1];
			memcpy(url, start, size);
			url[size] = '\0';
			
			for (size_t index = 0; index < size; index++) {
				char* offset = &url[index];
				
				if (size > (index + 6) && memcmp(offset, "\\u", 2) == 0) {
					const char c1 = from_hex(*(offset + 4));
					const char c2 = from_hex(*(offset + 5));
					
					*offset = (char) ((c1 << 4) | c2);
					memmove(offset + 1, offset + 6, strlen(offset + 6) + 1);
					
					size -= 5;
				}
			}
			
			char* const file_extension = get_file_extension(media_name);
			
			if (file_extension != NULL) {
				for (size_t index = 0; index < strlen(file_extension); index++) {
					char* ch = &file_extension[index];
					
					if (isupper(*ch)) {
						*ch = (char) tolower(*ch);
					}
				}
			}
			
			if (strcmp(media_type, "VIDEO") == 0) {
				curl_easy_setopt(curl_easy, CURLOPT_HTTPHEADER, NULL);
				curl_easy_setopt(curl_easy, CURLOPT_URL, url);
				
				string_free(&string);
				
				if (curl_easy_perform_retry(curl_easy) != CURLE_OK) {
					return UERR_CURL_FAILURE;
				}
				
				curl_easy_setopt(curl_easy, CURLOPT_URL, NULL);
				
				struct Tags tags = {0};
				
				if (m3u8_parse(&tags, string.s) != UERR_SUCCESS) {
					return UERR_M3U8_PARSE_FAILURE;
				}
				
				int last_width = 0;
				const char* playlist_uri = NULL;
				
				for (size_t index = 0; index < tags.offset; index++) {
					struct Tag* tag = &tags.items[index];
					
					if (tag->type != EXT_X_STREAM_INF) {
						continue;
					}
					
					const struct Attribute* const attribute = attributes_get(&tag->attributes, "RESOLUTION");
					
					const char* const start = attribute->value;
					const char* const end = strstr(start, "x");
					
					const size_t size = (size_t) (end - start);
					
					char value[size + 1];
					memcpy(value, start, size);
					value[size] = '\0';
					
					const int width = atoi(value);
					
					if (last_width < width) {
						last_width = width;
						playlist_uri = tag->uri;
					}
				}
				
				CURLU* cu __attribute__((__cleanup__(curlupp_free))) = curl_url();
				
				if (cu == NULL) {
					return UERR_CURLU_FAILURE;
				}
				
				if (curl_url_set(cu, CURLUPART_URL, url, 0) != CURLUE_OK) {
					return UERR_CURLU_FAILURE;
				}
				
				if (curl_url_set(cu, CURLUPART_URL, playlist_uri, 0) != CURLUE_OK) {
					return UERR_CURLU_FAILURE;
				}
				
				char* stream_url = NULL;
				
				if (curl_url_get(cu, CURLUPART_URL, &stream_url, 0) != CURLUE_OK) {
					return UERR_CURLU_FAILURE;
				}
				
				struct Media media = {
					.type = MEDIA_M3U8,
					.audio = {0},
					.video = {
						.id = malloc(strlen(media_code) + 1),
						.filename = malloc(strlen(media_name) + (file_extension == NULL ? strlen(DOT) + strlen(MP4_FILE_EXTENSION) : 0) + 1),
						.short_filename = malloc(strlen(media_code) + strlen(DOT) + (file_extension == NULL ? strlen(MP4_FILE_EXTENSION) : strlen(file_extension)) + 1),
						.url = malloc(strlen(stream_url) + 1)
					}
				};
				
				if (media.video.id == NULL || media.video.filename == NULL || media.video.short_filename == NULL || media.video.url == NULL) {
					return UERR_MEMORY_ALLOCATE_FAILURE;
				}
				
				strcpy(media.video.id, media_code);
				strcpy(media.video.url, stream_url);
				strcpy(media.video.filename, media_name);
				strcpy(media.video.short_filename, media_code);
				
				if (file_extension == NULL) {
					strcat(media.video.filename, DOT);
					strcat(media.video.filename, MP4_FILE_EXTENSION);
					strcat(media.video.short_filename, DOT);
					strcat(media.video.short_filename, MP4_FILE_EXTENSION);
				} else {
					strcat(media.video.short_filename, DOT);
					strcat(media.video.short_filename, file_extension);
				}
				
				normalize_filename(media.video.filename);
				
				page->medias.items[page->medias.offset++] = media;
			} else {
				struct Media media = {
					.type = MEDIA_SINGLE,
					.audio = {
						.id = malloc(strlen(media_code) + 1),
						.filename = malloc(strlen(media_name) + (file_extension == NULL ? strlen(DOT) + strlen(MP3_FILE_EXTENSION) : 0) + 1),
						.short_filename = malloc(strlen(media_code) + strlen(DOT) + (file_extension == NULL ? strlen(MP3_FILE_EXTENSION) : strlen(file_extension)) + 1),
						.url = malloc(strlen(url) + 1)
					},
					.video = {0}
				};
				
				if (media.audio.id == NULL || media.audio.filename == NULL || media.audio.short_filename == NULL || media.audio.url == NULL) {
					return UERR_MEMORY_ALLOCATE_FAILURE;
				}
				
				strcpy(media.audio.id, media_code);
				strcpy(media.audio.url, url);
				strcpy(media.audio.filename, media_name);
				strcpy(media.audio.short_filename, media_code);
				
				if (file_extension == NULL) {
					strcat(media.audio.filename, DOT);
					strcat(media.audio.filename, MP3_FILE_EXTENSION);
					strcat(media.audio.short_filename, DOT);
					strcat(media.audio.short_filename, MP3_FILE_EXTENSION);
				} else {
					strcat(media.audio.short_filename, DOT);
					strcat(media.audio.short_filename, file_extension);
				}
				
				normalize_filename(media.audio.filename);
				
				page->medias.items[page->medias.offset++] = media;
			}
			
			curl_easy_setopt(curl_easy, CURLOPT_HTTPHEADER, list);
		}
		
	}
	
	obj = json_object_get(tree, "attachments");
	
	if (obj != NULL) {
		if (!json_is_array(obj)) {
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		size_t index = 0;
		const json_t* item = NULL;
		const size_t array_size = json_array_size(obj);
		
		const size_t size = page->attachments.size + sizeof(struct Attachment) * array_size;
		struct Attachment* items = realloc(page->attachments.items, size);
		
		if (items == NULL) {
			return UERR_MEMORY_ALLOCATE_FAILURE;
		}
		
		page->attachments.size = size;
		page->attachments.items = items;
		
		json_array_foreach(obj, index, item) {
			if (!json_is_object(item)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const json_t* obj = json_object_get(item, "fileName");
			
			if (obj == NULL) {
				return UERR_JSON_MISSING_REQUIRED_KEY;
			}
			
			if (!json_is_string(obj)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const char* const filename = json_string_value(obj);
			
			obj = json_object_get(item, "fileMembershipId");
			
			if (obj == NULL) {
				return UERR_JSON_MISSING_REQUIRED_KEY;
			}
			
			if (!json_is_string(obj)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const char* const id = json_string_value(obj);
			
			char url[strlen(HOTMART_ATTACHMENT_ENDPOINT) + strlen(SLASH) + strlen(id) + strlen(SLASH) + 8 + 1];
			strcpy(url, HOTMART_ATTACHMENT_ENDPOINT);
			strcat(url, SLASH);
			strcat(url, id);
			strcat(url, SLASH);
			strcat(url, "download");
			
			struct String string __attribute__((__cleanup__(string_free))) = {0};
			
			curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, &string);
			curl_easy_setopt(curl_easy, CURLOPT_URL, url);
			
			if (curl_easy_perform_retry(curl_easy) != CURLE_OK) {
				return UERR_CURL_FAILURE;
			}
			
			json_auto_t* subtree = json_loads(string.s, 0, NULL);
			
			string_free(&string);
			
			if (tree == NULL) {
				return UERR_JSON_CANNOT_PARSE;
			}
			
			const char* download_url = NULL;
			
			obj = json_object_get(subtree, "token");
			
			if (obj == NULL) {
				obj = json_object_get(subtree, "directDownloadUrl");
				
				if (obj == NULL) {
					return UERR_JSON_MISSING_REQUIRED_KEY;
				}
				
				if (!json_is_string(obj)) {
					return UERR_JSON_NON_MATCHING_TYPE;
				}
				
				download_url = json_string_value(obj);
			} else {
				if (!json_is_string(obj)) {
					return UERR_JSON_NON_MATCHING_TYPE;
				}
				
				const char* const drm_token = json_string_value(obj);
				
				obj = json_object_get(subtree, "lambdaUrl");
				
				if (obj == NULL) {
					return UERR_JSON_MISSING_REQUIRED_KEY;
				}
				
				if (!json_is_string(obj)) {
					return UERR_JSON_NON_MATCHING_TYPE;
				}
				
				const char* const lambda_url = json_string_value(obj);
				
				struct curl_slist* sublist __attribute__((__cleanup__(curl_slistp_free_all))) = NULL;
				
				char header[strlen(HTTP_HEADER_TOKEN) + strlen(HTTP_HEADER_SEPARATOR) + strlen(drm_token) + 1];
				strcpy(header, HTTP_HEADER_TOKEN);
				strcat(header, HTTP_HEADER_SEPARATOR);
				strcat(header, drm_token);
				
				struct curl_slist* tmp = curl_slist_append(sublist, header);
				
				if (tmp == NULL) {
					return UERR_CURL_FAILURE;
				}
				
				sublist = tmp;
				
				curl_easy_setopt(curl_easy, CURLOPT_HTTPHEADER, sublist);
				curl_easy_setopt(curl_easy, CURLOPT_URL, lambda_url);
				
				if (curl_easy_perform_retry(curl_easy) != CURLE_OK) {
					return UERR_CURL_FAILURE;
				}
				
				if (!(string.slength > strlen(HTTPS_SCHEME) && memcmp(string.s, HTTPS_SCHEME, strlen(HTTPS_SCHEME)) == 0)) {
					return UERR_ATTACHMENT_DRM_FAILURE;
				}
				
				download_url = string.s;
				
				curl_easy_setopt(curl_easy, CURLOPT_HTTPHEADER, list);
				curl_easy_setopt(curl_easy, CURLOPT_URL, NULL);
			}
			
			const int hash = hashs(id);
			
			char sid[intlen(hash) + 1];
			snprintf(sid, sizeof(sid), "%i", hash);
			
			const char* const file_extension = get_file_extension(filename);
			
			struct Attachment attachment = {
				.id = malloc(strlen(sid) + 1),
				.filename = malloc(strlen(filename) + 1),
				.short_filename = malloc(strlen(sid) + (file_extension == NULL ? 0 : strlen(DOT) + strlen(file_extension))  + 1),
				.url = malloc(strlen(download_url) + 1),
			};
			
			if (attachment.id == NULL || attachment.filename == NULL || attachment.short_filename == NULL || attachment.url == NULL) {
				return UERR_MEMORY_ALLOCATE_FAILURE;
			}
			
			strcpy(attachment.id, sid);
			strcpy(attachment.url, download_url);
			strcpy(attachment.filename, filename);
			strcpy(attachment.short_filename, sid);
			
			if (file_extension != NULL) {
				strcat(attachment.short_filename, DOT);
				strcat(attachment.short_filename, file_extension);
			}
			
			normalize_filename(attachment.filename);
			
			page->attachments.items[page->attachments.offset++] = attachment;
		}
	}
	
	curl_easy_setopt(curl_easy, CURLOPT_HTTPHEADER, NULL);
	
	obj = json_object_get(tree, "content");
	
	if (obj != NULL) {
		if (!json_is_string(obj)) {
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		const char* const content = json_string_value(obj);
		
		const tidy_doc_t* const document __attribute__((__cleanup__(tidy_releasep))) = tidy_create();
		
		if (document == NULL) {
			return UERR_TIDY_FAILURE;
		}
		
		tidy_opt_set_bool(document, TidyShowWarnings, 0);
		
		tidy_buffer_t buffer __attribute__((__cleanup__(tidy_buffer_free))) = {0};
		tidy_buffer_init(&buffer);
		tidy_buffer_append(&buffer, (char*) content, (unsigned int) strlen(content));
		
		if (tidy_parse_buffer(document, &buffer) < 0) {
			return UERR_TIDY_FAILURE;
		}
		
		const tidy_node_t* const root = tidy_get_root(document);
		
		string_array_t items __attribute__((__cleanup__(string_array_free))) = {0};
		const int c = attribute_find_all(&items, root, "iframe", "src");
		
		if (c != UERR_SUCCESS) {
			return c;
		}
		
		for (size_t index = 0; index < items.offset; index++) {
			const char* const url = items.items[index];
			
			printf("+ A mídia localizada em '%s' aponta para uma fonte externa, verificando se é possível processá-la\r\n", url);
			
			struct Media media = {0};
			
			if (memcmp(url, VIMEO_URL_PATTERN, strlen(VIMEO_URL_PATTERN)) == 0) {
				char referer[strlen(resource->url) + strlen(SLASH) + strlen(HOTMART_EMBED_PAGE_PREFIX) + strlen(page->id) + 1];
				strcpy(referer, resource->url);
				strcat(referer, SLASH);
				strcat(referer, HOTMART_EMBED_PAGE_PREFIX);
				strcat(referer, page->id);
				
				const int code = vimeo_parse(url, resource, page, &media, referer);
				
				if (!(code == UERR_SUCCESS || code == UERR_NO_STREAMS_AVAILABLE)) {
					return code;
				}
			} else if (memcmp(url, YOUTUBE_URL_PATTERN, strlen(YOUTUBE_URL_PATTERN)) == 0) {
				const int code = youtube_parse(url, resource, page, &media, NULL);
				
				if (!(code == UERR_SUCCESS || code == UERR_NO_STREAMS_AVAILABLE)) {
					return code;
				}
			}
			
			if (media.video.url == NULL) {
				 fprintf(stderr, "- A URL é inválida ou não foi reconhecida. Por favor, reporte-a ao desenvolvedor.\r\n");
			} else {
				const size_t size = page->medias.size + sizeof(struct Media) * 1;
				struct Media* items = (struct Media*) realloc(page->medias.items, size);
				
				if (items == NULL) {
					return UERR_MEMORY_ALLOCATE_FAILURE;
				}
				
				page->medias.size = size;
				page->medias.items = items;
				
				page->medias.items[page->medias.offset++] = media;
				
			}
		}
		
		page->document.id = malloc(strlen(page->id) + 1);
		page->document.filename = malloc(strlen(page->name) + strlen(DOT) + strlen(HTML_FILE_EXTENSION) + 1);
		page->document.short_filename = malloc(strlen(page->id) + strlen(DOT) + strlen(HTML_FILE_EXTENSION) + 1);
		page->document.content = malloc(strlen(HTML_HEADER_START) + strlen(content) + strlen(HTML_HEADER_END) + 1);
		
		if (page->document.id == NULL || page->document.filename == NULL || page->document.short_filename == NULL || page->document.content == NULL) {
			return UERR_MEMORY_ALLOCATE_FAILURE;
		}
		
		strcpy(page->document.id, page->id);
		
		strcpy(page->document.filename, page->name);
		strcat(page->document.filename, DOT);
		strcat(page->document.filename, HTML_FILE_EXTENSION);
		
		strcpy(page->document.short_filename, page->id);
		strcat(page->document.short_filename, DOT);
		strcat(page->document.short_filename, HTML_FILE_EXTENSION);
		
		normalize_filename(page->document.filename);
		
		strcpy(page->document.content, HTML_HEADER_START);
		strcat(page->document.content, content);
		strcat(page->document.content, HTML_HEADER_END);
	}
	
	curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, NULL);
	curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, NULL);
	curl_easy_setopt(curl_easy, CURLOPT_URL, NULL);
	curl_easy_setopt(curl_easy, CURLOPT_REFERER, NULL);
	
	return UERR_SUCCESS;
	
}
