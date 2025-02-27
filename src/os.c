#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
	#include <windows.h>
#else
	#include <unistd.h>
	
	#if defined(__AppleiOS__) || defined(__AppleTV__)
		#include <spawn.h>
	#endif
	
	#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__AppleiOS__) || defined(__AppleTV__)
		#include <sys/wait.h>
	#endif
#endif

#include "symbols.h"
#include "os.h"

int execute_shell_command(const char* const command) {
	/*
	Executes a shell command.
	
	Returns the exit code for the command, which is (0) on success.
	*/
	
	#if defined(_WIN32) && defined(_UNICODE)
		const int wcommands = MultiByteToWideChar(CP_UTF8, 0, command, -1, NULL, 0);
		
		if (wcommands == 0) {
			return -1;
		}
		
		wchar_t wcommand[wcommands];
		
		if (MultiByteToWideChar(CP_UTF8, 0, command, -1, wcommand, wcommands) == 0) {
			return -1;
		}
		
		const int code = _wsystem(wcommand);
	#else
		#if defined(__AppleiOS__)
			char* const argv[] = {
				"sh",
				"-c",
				(char* const) command,
				NULL
			};
			
			pid_t pid = 0;
			
			if (posix_spawnp(&pid, "/bin/sh", 0, NULL, argv, NULL) != 0) {
				return -1;
			}
			
			int code = 0;
			
			do {
				const int status = waitpid(pid, &code, WUNTRACED | WCONTINUED);
				
				if (status == -1) {
					return -1;
				}
			} while (!WIFEXITED(code) && !WIFSIGNALED(code));
		#else
			const int code = system(command);
		#endif
	#endif
	
	#ifndef _WIN32
		const int exit_code = WIFSIGNALED(code) ? 128 + WTERMSIG(code) : WEXITSTATUS(code);
	#else
		const int exit_code = code;
	#endif
	
	return exit_code;
	
}

int is_administrator(void) {
	/*
	Returns whether the caller's process is a member of the Administrators local
	group (on Windows) or a root (on POSIX), via "geteuid() == 0".
	
	Returns (1) on true, (0) on false, (-1) on error.
	*/
	
	#ifdef _WIN32
		SID_IDENTIFIER_AUTHORITY authority = {
			.Value = SECURITY_NT_AUTHORITY
		};
		
		PSID group = {0};
		
		BOOL status = AllocateAndInitializeSid(
			&authority,
			2,
			SECURITY_BUILTIN_DOMAIN_RID,
			DOMAIN_ALIAS_RID_ADMINS,
			0,
			0,
			0,
			0,
			0,
			0,
			&group
		);
		
		if (status == 0) {
			return -1;
		}
		
		BOOL is_member = 0;
		status = CheckTokenMembership(0, group, &is_member);
		
		FreeSid(group);
		
		if (status == 0) {
			return -1;
		}
		
		return (int) is_member;
	#else
		return geteuid() == 0;
	#endif
	
}

char* get_configuration_directory(void) {
	/*
	Returns the config directory of the current user for applications.
	
	On non-Windows OSs, this proc conforms to the XDG Base Directory
	spec. Thus, this proc returns the value of the "XDG_CONFIG_HOME" environment
	variable if it is set, otherwise it returns the default configuration directory ("~/.config/").
	
	Returns NULL on error.
	*/
	
	#ifdef _WIN32
		#ifdef _UNICODE
			const wchar_t* const wdirectory = _wgetenv(L"APPDATA");
			
			if (wdirectory == NULL) {
				return NULL;
			}
			
			const int directorys = WideCharToMultiByte(CP_UTF8, 0, wdirectory, -1, NULL, 0, NULL, NULL);
			
			if (directorys == 0) {
				return NULL;
			}
			
			char directory[(size_t) directorys];
			
			if (WideCharToMultiByte(CP_UTF8, 0, wdirectory, -1, directory, directorys, NULL, NULL) == 0) {
				return NULL;
			}
		#else
			const char* const directory = getenv("APPDATA");
			
			if (directory == NULL) {
				return NULL;
			}
		#endif
	#else
		const char* const directory = getenv("XDG_CONFIG_HOME");
		
		if (directory == NULL) {
			const char* const config_directory = ".config";
			const char* const home_directory = getenv("HOME");
			
			char* configuration_directory = malloc(strlen(home_directory) + strlen(PATH_SEPARATOR) + strlen(config_directory) + 1);
			
			if (configuration_directory == NULL) {
				return NULL;
			}
			
			strcpy(configuration_directory, home_directory);
			strcat(configuration_directory, PATH_SEPARATOR);
			strcat(configuration_directory, config_directory);
			
			return configuration_directory;
		}
	#endif
	
	// Strip trailing path separator
	if (strlen(directory) > 1) {
		char* ptr = strchr(directory, '\0') - 1;
		
		if (*ptr == *PATH_SEPARATOR) {
			*ptr = '\0';
		}
	}
	
	char* configuration_directory = malloc(strlen(directory) + 1);
	
	if (configuration_directory == NULL) {
		return NULL;
	}
	
	strcpy(configuration_directory, directory);
	
	return configuration_directory;
	
}

char* get_temporary_directory(void) {
	/*
	Returns the temporary directory of the current user for applications to
	save temporary files in.
	
	On Windows, it calls GetTempPath().
	On Posix based platforms, it will check "TMPDIR", "TEMP", "TMP" and "TEMPDIR" environment variables in order.
	
	Returns NULL on error.
	*/
	
	#ifdef _WIN32
		#ifdef _UNICODE
			const size_t wdirectorys = (size_t) GetTempPathW(0, NULL);
			
			if (wdirectorys == 0) {
				return NULL;
			}
			
			wchar_t wdirectory[wdirectorys + 1];
			
			const DWORD code = GetTempPathW((DWORD) (sizeof(wdirectory) / sizeof(*wdirectory)), wdirectory);
			
			if (code == 0) {
				return 0;
			}
			
			const int directorys = WideCharToMultiByte(CP_UTF8, 0, wdirectory, -1, NULL, 0, NULL, NULL);
			
			if (directorys == 0) {
				return NULL;
			}
			
			char* temporary_directory = malloc((size_t) directorys);
			
			if (temporary_directory == NULL) {
				return NULL;
			}
			
			if (WideCharToMultiByte(CP_UTF8, 0, wdirectory, -1, temporary_directory, directorys, NULL, NULL) == 0) {
				return NULL;
			}
		#else
			const size_t directorys = (size_t) GetTempPathA(0, NULL);
			
			if (directorys == 0) {
				return NULL;
			}
			
			char directory[directorys + 1];
			const DWORD code = GetTempPathA((DWORD) sizeof(directory), directory);
			
			if (code == 0) {
				return 0;
			}
			
			char* temporary_directory = malloc(sizeof(directory));
			
			if (temporary_directory == NULL) {
				return NULL;
			}
			
			strcpy(temporary_directory, directory);
		#endif
	#else
		const char* const keys[] = {
			"TMPDIR",
			"TEMP",
			"TMP",
			"TEMPDIR"
		};
		
		for (size_t index = 0; index < (sizeof(keys) / sizeof(*keys)); index++) {
			const char* const key = keys[index];
			const char* const value = getenv(key);
			
			if (value == NULL) {
				continue;
			}
			
			char* temporary_directory = malloc(strlen(value) + 1);
			
			if (temporary_directory == NULL) {
				return NULL;
			}
			
			strcpy(temporary_directory, value);
			
			return temporary_directory;
		}
		
		const char* const tmp = "/tmp";
		
		char* temporary_directory = malloc(strlen(tmp) + 1);
		
		if (temporary_directory == NULL) {
			return NULL;
		}
		
		strcpy(temporary_directory, tmp);
	#endif
	
	// Strip trailing path separator
	if (strlen(temporary_directory) > 1) {
		char* ptr = strchr(temporary_directory, '\0') - 1;
		
		if (*ptr == *PATH_SEPARATOR) {
			*ptr = '\0';
		}
	}
	
	return temporary_directory;
	
}
