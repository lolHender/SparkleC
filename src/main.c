#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define A "SparkleC"

#ifdef _WIN32
	#include <stdio.h>
	#include <fcntl.h>
	#include <io.h>
	#include <locale.h>
#else
	#include <sys/resource.h>
#endif

#include <curl/curl.h>
#include <jansson.h>

#include "cleanup.h"
#include "errors.h"
#include "callbacks.h"
#include "m3u8.h"
#include "types.h"
#include "utils.h"
#include "symbols.h"
#include "curl.h"
#include "hotmart.h"
#include "fstream.h"
#include "providers.h"
#include "input.h"

#if defined(_WIN32) && defined(_UNICODE)
	#include "wio.h"
#endif

static const char LOCAL_ACCOUNTS_FILENAME[] = "accounts.json";

static void curl_poll(struct Download* dqueue, const size_t dcount, size_t* total_done) {
	
	CURLM* curl_multi = get_global_curl_multi();
	
	int still_running = 1;
	
	curl_progress_cb(NULL, (const curl_off_t) dcount, (const curl_off_t) *total_done, 0, 0);
	
	while (still_running) {
		CURLMcode mc = curl_multi_perform(curl_multi, &still_running);
		
		if (still_running) {
			mc = curl_multi_poll(curl_multi, NULL, 0, 1000, NULL);
		}
			
		CURLMsg* msg = NULL;
		int msgs_left = 0;
		
		int should_continue = 0;
		
		while ((msg = curl_multi_info_read(curl_multi, &msgs_left))) {
			if (msg->msg == CURLMSG_DONE) {
				struct Download* download = NULL;
				
				for (size_t index = 0; index < dcount; index++) {
					struct Download* subdownload = &dqueue[index];
					
					if (subdownload->handle == msg->easy_handle) {
						download = subdownload;
						break;
					}
				}
				
				curl_multi_remove_handle(curl_multi, msg->easy_handle);
				
				if (msg->data.result == CURLE_OK) {
					curl_easy_cleanup(msg->easy_handle);
					fstream_close(download->stream);
					
					(*total_done)++;
					curl_progress_cb(NULL, (const curl_off_t) dcount, (const curl_off_t) *total_done, 0, 0);
				} else {
					fstream_seek(download->stream, 0, FSTREAM_SEEK_BEGIN);
					curl_multi_add_handle(curl_multi, msg->easy_handle);
					
					if (!should_continue) {
						should_continue = 1;
					}
				}
			}
		}
		
		if (should_continue) {
			still_running = 1;
			continue;
		}
		
		if (mc) {
			break;
		}
	}
	
}

static int m3u8_download(const char* const url, const char* const output) {
	
	CURLM* curl_multi = get_global_curl_multi();
	CURL* curl_easy = get_global_curl_easy();
	
	struct String string __attribute__((__cleanup__(string_free))) = {0};
	
	curl_easy_setopt(curl_easy, CURLOPT_URL, url);
	curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, curl_write_string_cb);
	curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, &string);
	
	if (curl_easy_perform(curl_easy) != CURLE_OK) {
		return UERR_CURL_FAILURE;
	}
	
	struct Tags tags = {0};
	
	if (m3u8_parse(&tags, string.s) != UERR_SUCCESS) {
		fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
		return UERR_FAILURE;
	}
	
	int segment_number = 1;
	
	struct Download dl_queue[tags.offset];
	size_t dl_total = 0;
	size_t dl_done = 0;
	
	char playlist_filename[strlen(output) + strlen(DOT) + strlen(M3U8_FILE_EXTENSION) + 1];
	strcpy(playlist_filename, output);
	strcat(playlist_filename, DOT);
	strcat(playlist_filename, M3U8_FILE_EXTENSION);
	
	CURLU* cu __attribute__((__cleanup__(curlupp_free))) = curl_url();
	curl_url_set(cu, CURLUPART_URL, url, 0);
	
	for (size_t index = 0; index < tags.offset; index++) {
		struct Tag* tag = &tags.items[index];
		
		if (tag->type == EXT_X_KEY) {
			struct Attribute* attribute = attributes_get(&tag->attributes, "URI");
			
			curl_url_set(cu, CURLUPART_URL, url, 0);
			curl_url_set(cu, CURLUPART_URL, attribute->value, 0);
			
			char* key_url __attribute__((__cleanup__(curlcharpp_free))) = NULL;
			curl_url_get(cu, CURLUPART_URL, &key_url, 0);
			
			char* filename = malloc(strlen(output) + strlen(DOT) + strlen(KEY_FILE_EXTENSION) + 1);
			strcpy(filename, output);
			strcat(filename, DOT);
			strcat(filename, KEY_FILE_EXTENSION);
			
			attribute_set_value(attribute, filename);
			
			CURL* handle = curl_easy_new();
			
			if (handle == NULL) {
				fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
				return UERR_FAILURE;
			}
			
			curl_easy_setopt(handle, CURLOPT_URL, key_url);
			
			struct FStream* stream = fstream_open(filename, "wb");
			
			if (stream == NULL && errno == EMFILE) {
				curl_poll(dl_queue, dl_total, &dl_done);
				stream = fstream_open(filename, "wb");
			}
			
			if (stream == NULL) {
				fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o arquivo em '%s': %s\r\n", filename, strerror(errno));
				return UERR_FAILURE;
			}
			
			curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_write_file_cb);
			curl_easy_setopt(handle, CURLOPT_WRITEDATA, (void*) stream);
			curl_multi_add_handle(curl_multi, handle);
			
			struct Download download = {
				.handle = handle,
				.filename = filename,
				.stream = stream
			};
			
			dl_queue[dl_total++] = download;
			
			curl_url_set(cu, CURLUPART_URL, url, 0);
			curl_url_set(cu, CURLUPART_URL, tag->uri, 0);
			
			char* segment_url __attribute__((__cleanup__(curlcharpp_free))) = NULL;
			curl_url_get(cu, CURLUPART_URL, &segment_url, 0);
			
			handle = curl_easy_new();
			
			if (handle == NULL) {
				fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar inicializar o cliente HTTP!\r\n");
				return UERR_FAILURE;
			}
			
			curl_easy_setopt(handle, CURLOPT_URL, segment_url);
			
			char value[intlen(segment_number) + 1];
			snprintf(value, sizeof(value), "%i", segment_number);
			
			char* segment_filename = malloc(strlen(output) + strlen(DOT) + strlen(value) + strlen(DOT) + strlen(TS_FILE_EXTENSION) + 1);
			strcpy(segment_filename, output);
			strcat(segment_filename, DOT);
			strcat(segment_filename, value);
			strcat(segment_filename, DOT);
			strcat(segment_filename, TS_FILE_EXTENSION);
			
			tag_set_uri(tag, segment_filename);
			
			stream = fstream_open(segment_filename, "wb");
			
			if (stream == NULL && errno == EMFILE) {
				curl_poll(dl_queue, dl_total, &dl_done);
				stream = fstream_open(segment_filename, "wb");
			}
			
			if (stream == NULL) {
				fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o arquivo em '%s': %s\r\n", filename, strerror(errno));
				return UERR_FAILURE;
			}
			
			curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_write_file_cb);
			curl_easy_setopt(handle, CURLOPT_WRITEDATA, (void*) stream);
			curl_multi_add_handle(curl_multi, handle);
			
			download = (struct Download) {
				.handle = handle,
				.filename = segment_filename,
				.stream = stream
			};
			
			dl_queue[dl_total++] = download;
			
			segment_number++;
		} else if (tag->type == EXTINF && tag->uri != NULL) {
			curl_url_set(cu, CURLUPART_URL, url, 0);
			curl_url_set(cu, CURLUPART_URL, tag->uri, 0);
			
			char* segment_url __attribute__((__cleanup__(curlcharpp_free))) = NULL;
			curl_url_get(cu, CURLUPART_URL, &segment_url, 0);
			
			char value[intlen(segment_number) + 1];
			snprintf(value, sizeof(value), "%i", segment_number);
			
			char* filename = malloc(strlen(output) + strlen(DOT) + strlen(value) + strlen(DOT) + strlen(TS_FILE_EXTENSION) + 1);
			strcpy(filename, output);
			strcat(filename, DOT);
			strcat(filename, value);
			strcat(filename, DOT);
			strcat(filename, TS_FILE_EXTENSION);
			
			tag_set_uri(tag, filename);
			
			CURL* handle = curl_easy_new();
			
			if (handle == NULL) {
				fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
				return UERR_FAILURE;
			}
			
			curl_easy_setopt(handle, CURLOPT_URL, segment_url);
			
			struct FStream* stream = fstream_open(filename, "wb");
			
			if (stream == NULL && errno == EMFILE) {
				curl_poll(dl_queue, dl_total, &dl_done);
				stream = fstream_open(filename, "wb");
			}
			
			if (stream == NULL) {
				fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o arquivo em '%s': %s\r\n", filename, strerror(errno));
				return UERR_FAILURE;
			}
			
			curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_write_file_cb);
			curl_easy_setopt(handle, CURLOPT_WRITEDATA, (void*) stream);
			curl_multi_add_handle(curl_multi, handle);
			
			struct Download download = {
				.handle = handle,
				.filename = filename,
				.stream = stream
			};
			
			dl_queue[dl_total++] = download;
			
			segment_number++;
		}
		
	}
	
	curl_poll(dl_queue, dl_total, &dl_done);
	
	printf("+ Exportando lista de reprodução para '%s'\r\n", playlist_filename);
	 
	struct FStream* const stream = fstream_open(playlist_filename, "wb");
	
	if (stream == NULL) {
		fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o arquivo em '%s': %s\r\n", playlist_filename, strerror(errno));
		return UERR_FAILURE;
	}
			
	const int ok = tags_dumpf(&tags, stream);
	
	fstream_close(stream);
	m3u8_free(&tags);
	
	if (!ok) {
		fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar exportar a lista de reprodução!\r\n");
		return UERR_FAILURE;
	}
	
	printf("+ Copiando arquivos de mídia para '%s'\r\n", output);
	
	const char* const command = "ffmpeg -loglevel error -allowed_extensions ALL -i \"%s\" -c copy \"%s\"";
	
	const int size = snprintf(NULL, 0, command, playlist_filename, output);
	char shell_command[size + 1];
	snprintf(shell_command, sizeof(shell_command), command, playlist_filename, output);
	
	const int exit_code = execute_shell_command(shell_command);
	
	for (size_t index = 0; index < dl_total; index++) {
		struct Download* const download = &dl_queue[index];
		
		remove_file(download->filename);
		free(download->filename);
	}
	
	remove_file(playlist_filename);
	
	if (exit_code != 0) {
		fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar processar a mídia!\r\n");
		return UERR_FAILURE;
	}
	
	curl_easy_setopt(curl_easy, CURLOPT_URL, NULL);
	curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, NULL);
	curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, NULL);
	
	return UERR_SUCCESS;
	
}

#if defined(_WIN32) && defined(_UNICODE)
	#define main wmain
	int wmain(void);
#endif

int main(void) {
	
	#ifdef _WIN32
		_setmaxstdio(2048);
		
		#ifdef _UNICODE
			_setmode(_fileno(stdout), _O_WTEXT);
			_setmode(_fileno(stderr), _O_WTEXT);
			
			setlocale(LC_ALL, ".UTF8");
		#endif
	#else
		struct rlimit rlim = {0};
		getrlimit(RLIMIT_NOFILE, &rlim);
		
		rlim.rlim_cur = rlim.rlim_max;
		setrlimit(RLIMIT_NOFILE, &rlim);
	#endif
	
	if (is_administrator()) {
		fprintf(stderr, "- Você não precisa e nem deve executar este programa com privilégios elevados!\r\n");
		return EXIT_FAILURE;
	}
	
	printf("+ Selecione o seu provedor de serviços:\r\n\r\n");
	
	for (size_t index = 0; index < PROVIDERS_NUM; index++) {
		const struct Provider provider = PROVIDERS[index];
		printf("%zu. \r\nNome: %s\r\nURL: %s\r\n\r\n", index + 1, provider.label, provider.url);
	}
	
	int value = input_integer(1, (int) PROVIDERS_NUM);
	
	const struct Provider provider = PROVIDERS[value - 1];
	const struct ProviderMethods methods = provider.methods;
	
	char* const directory = get_configuration_directory();
	
	if (directory == NULL) {
		fprintf(stderr, "- Não foi possível obter um diretório de configurações válido!\r\n");
		return EXIT_FAILURE;
	}
	
	char configuration_directory[strlen(directory) + strlen(A) + strlen(PATH_SEPARATOR) + strlen(provider.label) + 1];
	strcpy(configuration_directory, directory);
	strcat(configuration_directory, A);
	strcat(configuration_directory, PATH_SEPARATOR);
	strcat(configuration_directory, provider.label);
	
	free(directory);
	
	if (!directory_exists(configuration_directory)) {
		fprintf(stderr, "- Diretório de configurações não encontrado, criando-o\r\n");
		
		if (!create_directory(configuration_directory)) {
			fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o diretório em '%s': %s\r\n", configuration_directory, strerror(errno));
			return EXIT_FAILURE;
		}
	}
	
	char accounts_file[strlen(configuration_directory) + strlen(PATH_SEPARATOR) + strlen(LOCAL_ACCOUNTS_FILENAME) + 1];
	strcpy(accounts_file, configuration_directory);
	strcat(accounts_file, PATH_SEPARATOR);
	strcat(accounts_file, LOCAL_ACCOUNTS_FILENAME);
	
	CURL* curl_easy = get_global_curl_easy();
	
	if (curl_easy == NULL) {
		fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar inicializar o cliente HTTP!\r\n");
		return EXIT_FAILURE;
	}
	
	CURLM* curl_multi = get_global_curl_multi();
	
	if (curl_multi == NULL) {
		fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar inicializar o cliente HTTP!\r\n");
		return EXIT_FAILURE;
	}
	
	struct Credentials credentials = {0};
	
	if (file_exists(accounts_file)) {
		struct FStream* const stream = fstream_open(accounts_file, "r");
		
		if (stream == NULL) {
			fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar abrir o arquivo em '%s': %s\r\n", accounts_file, strerror(errno));
			return EXIT_FAILURE;
		}
		
		const long long file_size = get_file_size(accounts_file);
		
		char buffer[(size_t) file_size];
		
		const ssize_t rsize = fstream_read(stream, buffer, sizeof(buffer));
		
		if ((size_t) rsize != sizeof(buffer)) {
			return UERR_FSTREAM_FAILURE;
		}
		
		fstream_close(stream);
		
		json_auto_t* tree = json_loadb(buffer, sizeof(buffer), 0, NULL);
		
		if (tree == NULL || !json_is_array(tree)) {
			fprintf(stderr, "- O arquivo de credenciais localizado em '%s' possui uma sintaxe inválida ou não reconhecida!\r\n", accounts_file);
			return EXIT_FAILURE;
		}
		
		const size_t total_items = json_array_size(tree);
		
		if (total_items < 1) {
			fprintf(stderr, "- O arquivo de credenciais localizado em '%s' não possui credenciais salvas!\r\n", accounts_file);
			return EXIT_FAILURE;
		}
		
		struct Credentials items[total_items];
		
		size_t index = 0;
		const json_t* item = NULL;
		
		printf("+ Como você deseja acessar este serviço?\r\n\r\n");
		printf("0.\r\nAdicionar e usar nova conta\r\n\r\n");
		
		json_array_foreach(tree, index, item) {
			json_t* subobj = json_object_get(item, "username");
			
			if (subobj == NULL || !json_is_string(subobj)) {
				fprintf(stderr, "- O arquivo de configurações localizado em '%s' possui um formato inválido!\r\n", accounts_file);
				return EXIT_FAILURE;
			}
			
			const char* const username = json_string_value(subobj);
			
			subobj = json_object_get(item, "access_token");
			
			if (subobj == NULL || !json_is_string(subobj)) {
				fprintf(stderr, "- O arquivo de configurações localizado em '%s' possui um formato inválido!\r\n", accounts_file);
				return EXIT_FAILURE;
			}
			
			const char* const access_token = json_string_value(subobj);
			
			struct Credentials credentials = {
				.access_token = malloc(strlen(access_token) + 1)
			};
			
			if (credentials.access_token == NULL) {
				fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar alocar memória do sistema!\r\n");
				return EXIT_FAILURE;
			}
			
			strcpy(credentials.access_token, access_token);
			
			items[index] = credentials;
			
			printf("%zu. \r\nAcessar usando a conta: '%s'\r\n\r\n", index + 1, username);
		}
		
		const int value = input_integer(0, (int) total_items);
		
		if (value == 0) {
			char username[MAX_INPUT_SIZE] = {'\0'};
			input("> Insira seu usuário: ", username);
			
			char password[MAX_INPUT_SIZE] = {'\0'};
			input("> Insira sua senha: ", password);
			
			const int code = (*methods.authorize)(username, password, &credentials);
			
			if (code != UERR_SUCCESS) {
				fprintf(stderr, "- Não foi possível realizar a autenticação: %s\r\n", strurr(code));
				return EXIT_FAILURE;
			}
			
			struct FStream* const stream = fstream_open(accounts_file, "wb");
			
			if (stream == NULL) {
				fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o arquivo em '%s': %s\r\n", accounts_file, strerror(errno));
				return EXIT_FAILURE;
			}
			
			json_auto_t* subtree = json_object();
			json_object_set_new(subtree, "username", json_string(credentials.username));
			json_object_set_new(subtree, "access_token", json_string(credentials.access_token));
			
			json_array_append(tree, subtree);
			
			char* const buffer = json_dumps(tree, JSON_COMPACT);
			
			if (buffer == NULL) {
				fprintf(stderr, "- Ocorreu uma falha ao tentar exportar o arquivo de credenciais!\r\n");
				return EXIT_FAILURE;
			}
			
			const int status = fstream_write(stream, buffer, strlen(buffer));
			const int cerrno = errno;
			
			free(buffer);
			fstream_close(stream);
			
			if (!status) {
				fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar exportar o arquivo de credenciais: %s\r\n", strerror(cerrno));
				return EXIT_FAILURE;
			}
		} else {
			credentials = items[value - 1];
		}
	} else {
		char username[MAX_INPUT_SIZE] = {'\0'};
		input("> Insira seu usuário: ", username);
		
		char password[MAX_INPUT_SIZE] = {'\0'};
		input("> Insira sua senha: ", password);
		
		const int code = (*methods.authorize)(username, password, &credentials);
		
		if (code != UERR_SUCCESS) {
			fprintf(stderr, "- Não foi possível realizar a autenticação: %s\r\n", strurr(code));
			return EXIT_FAILURE;
		}
		
		struct FStream* const stream = fstream_open(accounts_file, "wb");
		
		if (stream == NULL) {
			fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o arquivo em '%s': %s\r\n", accounts_file, strerror(errno));
			return EXIT_FAILURE;
		}
		
		json_auto_t* tree = json_array();
		json_auto_t* obj = json_object();
		
		if (tree == NULL || obj == NULL) {
			fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
			return EXIT_FAILURE;
		}
		
		json_object_set_new(obj, "username", json_string(credentials.username));
		json_object_set_new(obj, "access_token", json_string(credentials.access_token));
		
		json_array_append(tree, obj);
		
		char* const buffer = json_dumps(tree, JSON_COMPACT);
		
		if (buffer == NULL) {
			fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar exportar o arquivo de credenciais!\r\n");
			return EXIT_FAILURE;
		}
		
		const int status = fstream_write(stream, buffer, strlen(buffer));
		const int cerrno = errno;
		
		free(buffer);
		fstream_close(stream);
		
		if (!status) {
			fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar exportar o arquivo de credenciais: %s\r\n", strerror(cerrno));
			return EXIT_FAILURE;
		}
	}
	
	printf("+ Obtendo lista de conteúdos disponíveis\r\n");
	
	struct Resources resources = {0};
	
	const int code = (*methods.get_resources)(&credentials, &resources);
	
	if (code == UERR_HOTMART_SESSION_EXPIRED) {
		fprintf(stderr, "- Sua sessão expirou ou foi revogada, refaça o login!\r\n");
		return EXIT_FAILURE;
	}
	
	if (code != UERR_SUCCESS) {
		fprintf(stderr, "- Não foi possível obter a lista de produtos!\r\n");
		return EXIT_FAILURE;
	}
	
	if (resources.offset < 1) {
		fprintf(stderr, "- Não foram encontrados conteúdos disponíveis para baixar!\r\n");
		return EXIT_FAILURE;
	}
	
	printf("+ Selecione o que deseja baixar:\r\n\r\n");
	
	for (size_t index = 0; index < resources.offset; index++) {
		const struct Resource* resource = &resources.items[index];
		printf("%zu. \r\nNome: %s\r\nQualificação: %s\r\nURL: %s\r\n\r\n", index + 1, resource->name, resource->qualification.name == NULL ? "N/A" : resource->qualification.name, resource->url);
	}
	
	struct Resource download_queue[resources.offset];
	size_t queue_count = 0;
	
	while (1) {
		if (queue_count > 0) {
			queue_count = 0;
		}
		
		char query[MAX_INPUT_SIZE] = {'\0'};
		input("> Digite sua escolha: ", query);
		
		const char* start = query;
		int err = 0;
		
		for (size_t index = 0; index < strlen(query) + 1; index++) {
			const char* const ch = &query[index];
			
			if (!(*ch == *COMMA || (*ch == '\0' && index > 0))) {
				continue;
			}
			
			const size_t size = (size_t) (ch - start);
			
			if (size < 1) {
				fprintf(stderr, "- Não podem haver valores vazios dentre as escolhas!\r\n");
				err = 1;
				break;
			}
			
			char value[size + 1];
			memcpy(value, start, size);
			value[size] = '\0';
			
			const char* hyphen = strstr(value, HYPHEN);
			
			if (hyphen == NULL) {
				 if (!isnumeric(value)) {
					fprintf(stderr, "- O valor inserido é inválido ou não reconhecido!\r\n");
					err = 1;
					break;
				}
				
				const int position = atoi(value);
				
				if (position < 1) {
					fprintf(stderr, "- O valor mínimo de uma escolha deve ser >=1!\r\n");
					err = 1;
					break;
				}
				
				if (position > resources.offset) {
					fprintf(stderr, "- O valor máximo de uma escolha deve ser <=%zu!\r\n", resources.offset);
					err = 1;
					break;
				}
				
				struct Resource resource = resources.items[position - 1];
				
				for (size_t index = 0; index < queue_count; index++) {
					const struct Resource subresource = download_queue[index];
					
					if (subresource.id == resource.id) {
						fprintf(stderr, "- Não podem haver conteúdos duplicados dentre as escolhas!\r\n");
						err = 1;
						break;
					}
				}
				
				if (err) {
					break;
				}
				
				download_queue[queue_count++] = resource;
			} else {
				size_t size = (size_t) (hyphen - value);
				
				if (size < 1) {
					fprintf(stderr, "- O valor mínimo é obrigatório para intervalos de seleção!\r\n");
					err = 1;
					break;
				}
				
				char mins[size + 1];
				memcpy(mins, value, size);
				mins[size] = '\0';
				
				const int min = atoi(mins);
				
				if (min < 1) {
					fprintf(stderr, "- O valor mínimo para este intervalo deve ser >=1!\r\n");
					err = 1;
					break;
				}
				
				const char* const end = value + (sizeof(value) - 1);
				hyphen++;
				
				size = (size_t) (end - hyphen);
				
				if (size < 1) {
					fprintf(stderr, "- O valor máximo é obrigatório para intervalos de seleção!\r\n");
					err = 1;
					break;
				}
				
				char maxs[size + 1];
				memcpy(maxs, hyphen, size);
				maxs[size] = '\0';
				
				const int max = atoi(maxs);
				
				if (max > resources.offset) {
					fprintf(stderr, "- O valor máximo para este intervalo deve ser <=%zu!\r\n", resources.offset);
					err = 1;
					break;
				}
				
				for (size_t index = min; index < (max + 1); index++) {
					struct Resource resource = resources.items[index - 1];
					
					for (size_t index = 0; index < queue_count; index++) {
						const struct Resource subresource = download_queue[index];
						
						if (subresource.id == resource.id) {
							fprintf(stderr, "- Não podem haver conteúdos duplicados dentre as escolhas!\r\n");
							err = 1;
							break;
						}
					}
					
					if (err) {
						break;
					}
					
					download_queue[queue_count++] = resource;
				}
			}
			
			start = (ch + 1);
		}
		
		if (!err) {
			break;
		}
	}
	
	if (queue_count > 1) {
		printf("- %zu conteúdos foram enfileirados para serem baixados\r\n", queue_count);
	}
	
	int kof = 0;
	
	while (1) {
		printf("> Manter o nome original de arquivos e diretórios? (S/n) ");
		
		const char answer = (char) getchar();
		
		(void) getchar();
		
		switch (answer) {
			case 's':
			case 'y':
			case 'S':
			case 'Y':
				kof = 1;
				break;
			case 'n':
			case 'N':
				kof = 0;
				break;
			default:
				fprintf(stderr, "- O valor inserido é inválido ou não reconhecido!\r\n");
				continue;
		}
		
		break;
	}
	
	fclose(stdin);
	
	char* cwd = get_current_directory();
	
	if (cwd == NULL) {
		fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
		return EXIT_FAILURE;
	}
	
	const int has_trailing_sep = (strlen(cwd) > 0 && *(strchr(cwd, '\0') - 1) == *PATH_SEPARATOR);
	
	for (size_t index = 0; index < queue_count; index++) {
		struct Resource* resource = &download_queue[index];
		
		json_auto_t* modules = json_array();
		
		printf("+ Obtendo lista de módulos do produto '%s'\r\n", resource->name);
		
		const int code = (*methods.get_modules)(&credentials, resource);
		
		if (code != UERR_SUCCESS) {
			fprintf(stderr, "- Ocorreu uma falha inesperada: %s\r\n", strurr(code));
			return EXIT_FAILURE;
		}
		
		char directory[(kof ? strlen(resource->name) : strlen(resource->id)) + 1];
		strcpy(directory, kof ? resource->name : resource->id);
		normalize_filename(directory);
		
		char qualification_directory[(resource->qualification.id == NULL ? 0 : strlen(kof ? resource->qualification.name : resource->qualification.id)) + 1];
		*qualification_directory = '\0';
		
		if (resource->qualification.id != NULL) {
			strcat(qualification_directory, kof ? resource->qualification.name : resource->qualification.id);
			normalize_filename(qualification_directory);
		}
		
		char resource_directory[strlen(cwd) + (has_trailing_sep ? 0 : strlen(PATH_SEPARATOR)) + (*qualification_directory == '\0' ? 0 : strlen(qualification_directory) + strlen(PATH_SEPARATOR)) + strlen(directory) + 1];
		strcpy(resource_directory, cwd);
		
		if (!has_trailing_sep) {
			strcat(resource_directory, PATH_SEPARATOR);
		}
		
		if (resource->qualification.id != NULL) {
			strcat(resource_directory, qualification_directory);
			strcat(resource_directory, PATH_SEPARATOR);
		}
		
		strcat(resource_directory, directory);
		
		if (!directory_exists(resource_directory)) {
			fprintf(stderr, "- O diretório '%s' não existe, criando-o\r\n", resource_directory);
			
			if (!create_directory(resource_directory)) {
				fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o diretório em '%s': %s\r\n", resource_directory, strerror(errno));
				return EXIT_FAILURE;
			}
		}
		
		for (size_t index = 0; index < resource->modules.offset; index++) {
			struct Module* module = &resource->modules.items[index];
			
			printf("+ Verificando estado do módulo '%s'\r\n", module->name);
			
			if (module->is_locked) {
				fprintf(stderr, "- Módulo inacessível, pulando para o próximo\r\n");
				continue;
			}
			
			char directory[(kof ? strlen(module->name) : strlen(module->id)) + 1];
			strcpy(directory, (kof ? module->name : module->id));
			normalize_filename(directory);
			
			char module_directory[strlen(resource_directory) + strlen(PATH_SEPARATOR) + strlen(directory) + 1];
			strcpy(module_directory, resource_directory);
			strcat(module_directory, PATH_SEPARATOR);
			strcat(module_directory, directory);
			
			if (!directory_exists(module_directory)) {
				fprintf(stderr, "- O diretório '%s' não existe, criando-o\r\n", module_directory);
				
				if (!create_directory(module_directory)) {
					fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o diretório em '%s': %s\r\n", module_directory, strerror(errno));
					return EXIT_FAILURE;
				}
			}
			
			json_t* pages = json_array();
			
			curl_easy_setopt(curl_easy, CURLOPT_TIMEOUT, 0L);
			curl_easy_setopt(curl_easy, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
			
			int suffix = 0;
			
			for (size_t index = 0; index < module->attachments.offset; index++) {
				struct Attachment* attachment = &module->attachments.items[index];
				
				const char* const extension = get_file_extension(attachment->filename);
				suffix++;
				
				char attachment_filename[strlen(module_directory) + strlen(PATH_SEPARATOR) + (kof ? strlen(attachment->filename) : strlen(module->id) + intlen(suffix) + strlen(DOT) + strlen(extension)) + 1];
				strcpy(attachment_filename, module_directory);
				strcat(attachment_filename, PATH_SEPARATOR);
				
				if (kof) {
					strcat(attachment_filename, attachment->filename);
				} else {
					strcat(attachment_filename, module->id);
					
					char value[intlen(suffix) + 1];
					snprintf(value, sizeof(value), "%i", suffix);
					
					strcat(attachment_filename, value);
					strcat(attachment_filename, DOT);
					strcat(attachment_filename, extension);
					
					normalize_filename(basename(attachment_filename));
				}
				
				json_t* content = json_object();
				json_object_set_new(content, "type", json_string("file"));
				json_object_set_new(content, "name", json_string(attachment->filename));
				json_object_set_new(content, "path", json_string(attachment_filename));
				
				json_array_append_new(pages, content);
				
				if (!file_exists(attachment_filename)) {
					fprintf(stderr, "- O arquivo '%s' não existe, ele será baixado\r\n", attachment_filename);
					printf("+ Baixando de '%s' para '%s'\r\n", attachment->url, attachment_filename);
					
					struct FStream* const stream = fstream_open(attachment_filename, "wb");
					
					if (stream == NULL) {
						fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o arquivo em '%s': %s\r\n", attachment_filename, strerror(errno));
						return EXIT_FAILURE;
					}
					
					curl_easy_setopt(curl_easy, CURLOPT_NOPROGRESS, 0L);
					curl_easy_setopt(curl_easy, CURLOPT_URL, attachment->url);
					curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, curl_write_file_cb);
					curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, (void*) stream);
					curl_easy_setopt(curl_easy, CURLOPT_FOLLOWLOCATION, 1L);
					
					const CURLcode code = curl_easy_perform(curl_easy);
					
					printf("\n");
					
					fstream_close(stream);
					
					if (code != CURLE_OK) {
						remove_file(attachment_filename);
						
						fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar conectar com o servidor em '%s': %s\r\n", attachment->url, curl_easy_strerror(code));
						return EXIT_FAILURE;
					}
				}
			}
			
			curl_easy_setopt(curl_easy, CURLOPT_URL, NULL);
			curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, NULL);
			curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, NULL);
			curl_easy_setopt(curl_easy, CURLOPT_NOPROGRESS, 1L);
			curl_easy_setopt(curl_easy, CURLOPT_TIMEOUT, 60L);
			curl_easy_setopt(curl_easy, CURLOPT_XFERINFOFUNCTION, NULL);
			curl_easy_setopt(curl_easy, CURLOPT_FOLLOWLOCATION, 0L);
			
			printf("+ Obtendo lista de páginas do módulo '%s'\r\n", module->name);
			
			for (size_t index = 0; index < module->pages.offset; index++) {
				struct Page* page = &module->pages.items[index];
				
				printf("+ Obtendo informações sobre a página '%s'\r\n", page->name);
				
				if (page->is_locked) {
					fprintf(stderr, "- Página inacessível, pulando para a próxima\r\n");
					continue;
				}
				
				const int code = (*methods.get_page)(&credentials, resource, page);
				
				switch (code) {
					case UERR_SUCCESS:
						break;
					case UERR_NOT_IMPLEMENTED:
						fprintf(stderr, "- As informações sobre esta página já foram obtidas, pulando etapa\r\n");
						break;
					default:
						fprintf(stderr, "- Ocorreu uma falha inesperada: %s\r\n", strurr(code));
						return EXIT_FAILURE;
				}
				
				printf("+ Verificando estado da página '%s'\r\n", page->name);
				
				char directory[(kof ? strlen(page->name) : strlen(page->id)) + 1];
				strcpy(directory, (kof ? page->name : page->id));
				normalize_filename(directory);
				
				char page_directory[strlen(module_directory) + strlen(PATH_SEPARATOR) + strlen(directory) + 1];
				strcpy(page_directory, module_directory);
				strcat(page_directory, PATH_SEPARATOR);
				strcat(page_directory, directory);
				
				json_t* const items = json_array();
				
				if (!directory_exists(page_directory)) {
					fprintf(stderr, "- O diretório '%s' não existe, criando-o\r\n", page_directory);
					
					if (!create_directory(page_directory)) {
						fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o diretório em '%s': %s\r\n", page_directory, strerror(errno));
						return EXIT_FAILURE;
					}
				}
				
				if (page->document.content != NULL) {
					const char* const extension = get_file_extension(page->document.filename);
					suffix++;
					
					char document_filename[strlen(page_directory) + strlen(PATH_SEPARATOR) + (kof ? strlen(page->document.filename) : strlen(page->id) + intlen(suffix) + strlen(DOT) + strlen(extension)) + 1];
					strcpy(document_filename, page_directory);
					strcat(document_filename, PATH_SEPARATOR);
					
					if (kof) {
						strcat(document_filename, page->document.filename);
					} else {
						strcat(document_filename, page->id);
						
						char value[intlen(suffix) + 1];
						snprintf(value, sizeof(value), "%i", suffix);
						
						strcat(document_filename, value);
						strcat(document_filename, DOT);
						strcat(document_filename, extension);
						
						normalize_filename(basename(document_filename));
					}
					
					json_t* const content = json_object();
					json_object_set_new(content, "type", json_string("file"));
					json_object_set_new(content, "name", json_string(page->document.filename));
					json_object_set_new(content, "path", json_string(document_filename));
					
					json_array_append_new(items, content);
					
					if (!file_exists(document_filename)) {
						fprintf(stderr, "- O arquivo '%s' não existe, salvando-o\r\n", document_filename);
						
						struct FStream* const stream = fstream_open(document_filename, "wb");
						
						if (stream == NULL) {
							fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o arquivo em '%s': %s\r\n", document_filename, strerror(errno));
							return EXIT_FAILURE;
						}
						
						const int status = fstream_write(stream, page->document.content, strlen(page->document.content));
						const int cerrno = errno;
						
						fstream_close(stream);
						
						if (!status) {
							remove_file(document_filename);
							fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar salvar o documento: %s\r\n", strerror(cerrno));
							return EXIT_FAILURE;
						}
					}
				}
				
				for (size_t index = 0; index < page->medias.offset; index++) {
					struct Media* media = &page->medias.items[index];
					
					const char* const extension = get_file_extension(media->video.filename);
					suffix++;
					
					char media_filename[strlen(page_directory) + strlen(PATH_SEPARATOR) + (kof ? strlen(media->video.filename) : strlen(page->id) + intlen(suffix) + strlen(DOT) + strlen(extension)) + 1];
					strcpy(media_filename, page_directory);
					strcat(media_filename, PATH_SEPARATOR);
					
					if (kof) {
						strcat(media_filename, media->video.filename);
					} else {
						strcat(media_filename, page->id);
						
						char value[intlen(suffix) + 1];
						snprintf(value, sizeof(value), "%i", suffix);
						
						strcat(media_filename, value);
						strcat(media_filename, DOT);
						strcat(media_filename, extension);
						
						normalize_filename(basename(media_filename));
					}
					
					json_t* const content = json_object();
					json_object_set_new(content, "type", json_string("file"));
					json_object_set_new(content, "name", json_string(media->video.filename));
					json_object_set_new(content, "path", json_string(media_filename));
					
					json_array_append_new(items, content);
					
					if (!file_exists(media_filename)) {
						fprintf(stderr, "- O arquivo '%s' não existe, baixando-o\r\n", media_filename);
						
						switch (media->type) {
							case MEDIA_M3U8: {
								char* audio_filename = NULL;
								const char* video_filename = NULL;
								
								if (media->audio.url != NULL) {
									const char* const extension = get_file_extension(media->audio.filename);
									suffix++;
									
									audio_filename = malloc(strlen(page_directory) + strlen(PATH_SEPARATOR) + (kof ? strlen(media->audio.filename) : strlen(page->id) + intlen(suffix) + strlen(DOT) + strlen(extension)) + 1);
									
									if (audio_filename == NULL) {
										fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar alocar memória do sistema!\r\n");
										return EXIT_FAILURE;
									}
									
									strcpy(audio_filename, page_directory);
									strcat(audio_filename, PATH_SEPARATOR);
									
									if (kof) {
										strcat(audio_filename, media->audio.filename);
									} else {
										strcat(audio_filename, page->id);
										
										char value[intlen(suffix) + 1];
										snprintf(value, sizeof(value), "%i", suffix);
										
										strcat(audio_filename, value);
										strcat(audio_filename, DOT);
										strcat(audio_filename, extension);
										
										normalize_filename(basename(audio_filename));
									}
					
									printf("+ Baixando de '%s' para '%s'\r\n", media->audio.url, audio_filename);
									
									if (m3u8_download(media->audio.url, audio_filename) != UERR_SUCCESS) {
										return EXIT_FAILURE;
									}
								}
								
								if (media->video.url != NULL) {
									printf("+ Baixando de '%s' para '%s'\r\n", media->video.url, media_filename);
									
									if (m3u8_download(media->video.url, media_filename) != UERR_SUCCESS) {
										return EXIT_FAILURE;
									}
									
									video_filename = media_filename;
								}
								
								if (audio_filename != NULL && video_filename != NULL) {
									const char* const extension = get_file_extension(video_filename);
									
									char temporary_file[strlen(video_filename) + strlen(DOT) + strlen(extension) + 1];
									strcpy(temporary_file, video_filename);
									strcat(temporary_file, DOT);
									strcat(temporary_file, extension);
									
									const char* const command = "ffmpeg -loglevel error -i \"%s\" -i \"%s\" -c copy -map 0:v:0 -map 1:a:0 \"%s\"";
									
									const int size = snprintf(NULL, 0, command, video_filename, audio_filename, temporary_file);
									char shell_command[size + 1];
									snprintf(shell_command, sizeof(shell_command), command, video_filename, audio_filename, temporary_file);
									
									printf("+ Copiando canais de áudio e vídeo para uma única mídia em '%s'\r\n", temporary_file);
									
									const int exit_code = execute_shell_command(shell_command);
									
									remove_file(audio_filename);
									remove_file(video_filename);
									
									free(audio_filename);
									audio_filename = NULL;
									
									if (exit_code != 0) {
										fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar processar a mídia!\r\n");
										return EXIT_FAILURE;
									}
									
									printf("+ Movendo arquivo de mídia de '%s' para '%s'\r\n", temporary_file, video_filename);
									
									move_file(temporary_file, video_filename);
								}
							}
							
							curl_easy_setopt(curl_easy, CURLOPT_URL, NULL);
							curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, NULL);
							curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, NULL);
							
							break;
							
							case MEDIA_SINGLE: {
								printf("+ Baixando de '%s' para '%s'\r\n", media->video.url, media_filename);
								
								struct FStream* const stream = fstream_open(media_filename, "wb");
								
								if (stream == NULL) {
									fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o arquivo em '%s': %s\r\n", media_filename, strerror(errno));
									return EXIT_FAILURE;
								}
								
								curl_easy_setopt(curl_easy, CURLOPT_TIMEOUT, 0L);
								curl_easy_setopt(curl_easy, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
								curl_easy_setopt(curl_easy, CURLOPT_NOPROGRESS, 0L);
								curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, curl_write_file_cb);
								curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, (void*) stream);
								curl_easy_setopt(curl_easy, CURLOPT_URL, media->video.url);
								curl_easy_setopt(curl_easy, CURLOPT_FOLLOWLOCATION, 1L);
								
								const CURLcode code = curl_easy_perform(curl_easy);
								
								printf("\n");
								
								fstream_close(stream);
								
								curl_easy_setopt(curl_easy, CURLOPT_XFERINFOFUNCTION, NULL);
								curl_easy_setopt(curl_easy, CURLOPT_NOPROGRESS, 1L);
								curl_easy_setopt(curl_easy, CURLOPT_TIMEOUT, 60L);
								curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, NULL);
								curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, NULL);
								curl_easy_setopt(curl_easy, CURLOPT_URL, NULL);
								curl_easy_setopt(curl_easy, CURLOPT_FOLLOWLOCATION, 0L);
								
								if (code != CURLE_OK) {
									remove_file(media_filename);
									fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar conectar com o servidor em '%s': %s\r\n", media->video.url, curl_easy_strerror(code));
									return EXIT_FAILURE;
								}
							}
							
							break;
						}
					}
				}
				
				curl_easy_setopt(curl_easy, CURLOPT_TIMEOUT, 0L);
				curl_easy_setopt(curl_easy, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
				
				for (size_t index = 0; index < page->attachments.offset; index++) {
					struct Attachment* attachment = &page->attachments.items[index];
					
					const char* const extension = get_file_extension(attachment->filename);
					suffix++;
					
					char attachment_filename[strlen(page_directory) + strlen(PATH_SEPARATOR) + (kof ? strlen(attachment->filename) : strlen(page->id) + intlen(suffix) + strlen(DOT) + strlen(extension)) + 1];
					strcpy(attachment_filename, page_directory);
					strcat(attachment_filename, PATH_SEPARATOR);
					
					if (kof) {
						strcat(attachment_filename, attachment->filename);
					} else {
						strcat(attachment_filename, page->id);
						
						char value[intlen(suffix) + 1];
						snprintf(value, sizeof(value), "%i", suffix);
						
						strcat(attachment_filename, value);
						strcat(attachment_filename, DOT);
						strcat(attachment_filename, extension);
						
						normalize_filename(basename(attachment_filename));
					}
					
					json_t* content = json_object();
					json_object_set_new(content, "type", json_string("file"));
					json_object_set_new(content, "name", json_string(attachment->filename));
					json_object_set_new(content, "path", json_string(attachment_filename));
					
					json_array_append_new(items, content);
					
					if (!file_exists(attachment_filename)) {
						fprintf(stderr, "- O arquivo '%s' não existe, ele será baixado\r\n", attachment_filename);
						printf("+ Baixando de '%s' para '%s'\r\n", attachment->url, attachment_filename);
						
						struct FStream* const stream = fstream_open(attachment_filename, "wb");
						
						if (stream == NULL) {
							fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o arquivo em '%s': %s\r\n", attachment_filename, strerror(errno));
							return EXIT_FAILURE;
						}
						
						curl_easy_setopt(curl_easy, CURLOPT_NOPROGRESS, 0L);
						curl_easy_setopt(curl_easy, CURLOPT_URL, attachment->url);
						curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, curl_write_file_cb);
						curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, (void*) stream);
						curl_easy_setopt(curl_easy, CURLOPT_FOLLOWLOCATION, 1L);
						
						const CURLcode code = curl_easy_perform(curl_easy);
						
						printf("\n");
						
						fstream_close(stream);
						
						if (code != CURLE_OK) {
							remove_file(attachment_filename);
							
							fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar conectar com o servidor em '%s': %s\r\n", attachment->url, curl_easy_strerror(code));
							return EXIT_FAILURE;
						}
					}
				}
				
				curl_easy_setopt(curl_easy, CURLOPT_URL, NULL);
				curl_easy_setopt(curl_easy, CURLOPT_WRITEFUNCTION, NULL);
				curl_easy_setopt(curl_easy, CURLOPT_WRITEDATA, NULL);
				curl_easy_setopt(curl_easy, CURLOPT_NOPROGRESS, 1L);
				curl_easy_setopt(curl_easy, CURLOPT_TIMEOUT, 60L);
				curl_easy_setopt(curl_easy, CURLOPT_XFERINFOFUNCTION, NULL);
				curl_easy_setopt(curl_easy, CURLOPT_FOLLOWLOCATION, 0L);
				
				json_t* tree = json_object();
				json_object_set_new(tree, "type", json_string("directory"));
				json_object_set_new(tree, "name", json_string(page->name));
				json_object_set_new(tree, "path", json_string(page_directory));
				json_object_set_new(tree, "items", items);
				
				json_array_append_new(pages, tree);
			}
			
			json_auto_t* tree = json_object();
			json_object_set_new(tree, "type", json_string("directory"));
			json_object_set_new(tree, "name", json_string(module->name));
			json_object_set_new(tree, "path", json_string(module_directory));
			json_object_set_new(tree, "items", pages);
			
			json_array_append(modules, tree);
		}
		
		char* const buffer = json_dumps(modules, JSON_COMPACT);
		
		if (buffer == NULL) {
			fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar gerar a árvore de objetos!\r\n");
			return EXIT_FAILURE;
		}
		
		char json_tree_filename[strlen(resource_directory) + strlen(DOT) + strlen(JSON_FILE_EXTENSION) + 1];
		strcpy(json_tree_filename, resource_directory);
		strcat(json_tree_filename, DOT);
		strcat(json_tree_filename, JSON_FILE_EXTENSION);
		
		printf("+ Exportando árvore de objetos para '%s'\r\n", json_tree_filename);
		
		struct FStream* stream = fstream_open(json_tree_filename, "wb");
		
		if (stream == NULL) {
			fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o arquivo em '%s': %s\r\n", json_tree_filename, strerror(errno));
			return UERR_FAILURE;
		}
		
		const int status = fstream_write(stream, buffer, strlen(buffer));
		const int cerrno = errno;
		
		fstream_close(stream);
		
		if (!status) {
			remove_file(json_tree_filename);
			fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar salvar o documento em '%s': %s\r\n", json_tree_filename, strerror(cerrno));
			return EXIT_FAILURE;
		}
		
		char html_tree_filename[strlen(resource_directory) + strlen(DOT) + strlen(HTML_FILE_EXTENSION) + 1];
		strcpy(html_tree_filename, resource_directory);
		strcat(html_tree_filename, DOT);
		strcat(html_tree_filename, HTML_FILE_EXTENSION);
		
		printf("+ Exportando árvore de objetos para '%s'\r\n", html_tree_filename);
		
		stream = fstream_open(html_tree_filename, "wb");
		
		if (stream == NULL) {
			fprintf(stderr, "- Ocorreu uma falha inesperada ao tentar criar o arquivo em '%s': %s\r\n", html_tree_filename, strerror(errno));
			return UERR_FAILURE;
		}
		
		fstream_write(stream, HTML_HEADER_START, strlen(HTML_HEADER_START));
		fstream_write(stream, HTML_UL_START, strlen(HTML_UL_START));
		
		size_t index = 0;
		const json_t* item = NULL;
		
		json_array_foreach(modules, index, item) {
			const json_t* obj = json_object_get(item, "name");
			const char* const name = json_string_value(obj);
			
			obj = json_object_get(item, "path");
			const char* const path = json_string_value(obj);
			
			char uri[strlen(FILE_SCHEME) + strlen(directory) + 1];
			strcpy(uri, FILE_SCHEME);
			strcat(uri, path);
			
			fstream_write(stream, HTML_LI_START, strlen(HTML_LI_START));
			
			fstream_write(stream, HTML_A_START, strlen(HTML_A_START));
			fstream_write(stream, SPACE, strlen(SPACE));
			fstream_write(stream, HTML_HREF_ATTRIBUTE, strlen(HTML_HREF_ATTRIBUTE));
			fstream_write(stream, EQUAL, strlen(EQUAL));
			fstream_write(stream, QUOTATION_MARK, strlen(QUOTATION_MARK));
			fstream_write(stream, uri, strlen(uri));
			fstream_write(stream, QUOTATION_MARK, strlen(QUOTATION_MARK));
			fstream_write(stream, GREATER_THAN, strlen(GREATER_THAN));
			fstream_write(stream, name, strlen(name));
			fstream_write(stream, HTML_A_END, strlen(HTML_A_END));
			
			fstream_write(stream, HTML_UL_START, strlen(HTML_UL_START));
			
			size_t index = 0;
			const json_t* page = NULL;
			
			const json_t* const pages = json_object_get(item, "items");
			
			json_array_foreach(pages, index, page) {
				const json_t* obj = json_object_get(page, "name");
				const char* const name = json_string_value(obj);
				
				obj = json_object_get(page, "path");
				const char* const path = json_string_value(obj);
				
				char uri[strlen(FILE_SCHEME) + strlen(directory) + 1];
				strcpy(uri, FILE_SCHEME);
				strcat(uri, path);
				
				fstream_write(stream, HTML_LI_START, strlen(HTML_LI_START));
				
				fstream_write(stream, HTML_A_START, strlen(HTML_A_START));
				fstream_write(stream, SPACE, strlen(SPACE));
				fstream_write(stream, HTML_HREF_ATTRIBUTE, strlen(HTML_HREF_ATTRIBUTE));
				fstream_write(stream, EQUAL, strlen(EQUAL));
				fstream_write(stream, QUOTATION_MARK, strlen(QUOTATION_MARK));
				fstream_write(stream, uri, strlen(uri));
				fstream_write(stream, QUOTATION_MARK, strlen(QUOTATION_MARK));
				fstream_write(stream, GREATER_THAN, strlen(GREATER_THAN));
				fstream_write(stream, name, strlen(name));
				fstream_write(stream, HTML_A_END, strlen(HTML_A_END));
				
				fstream_write(stream, HTML_UL_START, strlen(HTML_UL_START));
				
				const json_t* contents = json_object_get(page, "items");
				
				if (contents != NULL) {
					size_t index = 0;
					const json_t* content = NULL;
					
					json_array_foreach(contents, index, content) {
						const json_t* obj = json_object_get(content, "name");
						const char* const name = json_string_value(obj);
						
						obj = json_object_get(content, "path");
						const char* const path = json_string_value(obj);
						
						char uri[strlen(FILE_SCHEME) + strlen(directory) + 1];
						strcpy(uri, FILE_SCHEME);
						strcat(uri, path);
						
						fstream_write(stream, HTML_LI_START, strlen(HTML_LI_START));
						
						fstream_write(stream, HTML_A_START, strlen(HTML_A_START));
						fstream_write(stream, SPACE, strlen(SPACE));
						fstream_write(stream, HTML_HREF_ATTRIBUTE, strlen(HTML_HREF_ATTRIBUTE));
						fstream_write(stream, EQUAL, strlen(EQUAL));
						fstream_write(stream, QUOTATION_MARK, strlen(QUOTATION_MARK));
						fstream_write(stream, uri, strlen(uri));
						fstream_write(stream, QUOTATION_MARK, strlen(QUOTATION_MARK));
						fstream_write(stream, GREATER_THAN, strlen(GREATER_THAN));
						fstream_write(stream, name, strlen(name));
						fstream_write(stream, HTML_A_END, strlen(HTML_A_END));
						
						fstream_write(stream, HTML_LI_END, strlen(HTML_LI_END));
					}
				}
				
				fstream_write(stream, HTML_LI_END, strlen(HTML_LI_END));
				fstream_write(stream, HTML_UL_END, strlen(HTML_UL_END));
			}
			
			fstream_write(stream, HTML_LI_END, strlen(HTML_LI_END));
			fstream_write(stream, HTML_UL_END, strlen(HTML_UL_END));
		}
		
		fstream_write(stream, HTML_UL_END, strlen(HTML_UL_END));
		fstream_write(stream, HTML_HEADER_END, strlen(HTML_HEADER_END));
		
		fstream_close(stream);
	}
	
	return EXIT_SUCCESS;
	
}
