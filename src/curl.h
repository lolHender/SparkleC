#include <curl/curl.h>

CURL* get_global_curl_easy(void);
CURL* curl_easy_new(void);

CURLM* get_global_curl_multi(void);

const char* get_global_curl_error(void);

CURLcode curl_easy_perform_retry(CURL* const curl);