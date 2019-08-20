// vi: noexpandtab tabstop=4 softtabstop=4 shiftwidth=0 list
#include "util.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#	include <windows.h>
#else
#	include <unistd.h>
#	include <sys/stat.h>
#	include <errno.h>
#endif


#ifdef _WIN32

size_t
sys_utf8_from_wchar(wchar_t const *in, char *out, size_t bytes)
{
	assert(in);
	assert(bytes == 0 || out);
	//  CAST bytes: size_t -> int = assert range
	assert(bytes <= INT_MAX);
	//  CAST retval: int -> size_t = WCTMB always returns >= 0 (error == 0)
	return (size_t)WideCharToMultiByte(
		CP_UTF8,
		0,
		in,
		-1, // length of 'in'. -1: NUL-terminated
		out,
		(int)bytes,
		NULL, // must be set to 0 for CP_UTF8
		NULL /* must be set to 0 for CP_UTF8 */);
}


size_t
sys_wchar_from_utf8(char const *in, wchar_t *out, size_t chars)
{
	// CAST chars: size_t -> int = assert range
	assert(chars <= INT_MAX);
	// CAST retval: int -> size_t = MBTWC always returns >= 0 (error == 0)
	return (size_t)MultiByteToWideChar(
		CP_UTF8,
		0, // MB_PRECOMPOSED is default.
		in,
		-1, // length of 'in'. -1: NUL-terminated
		out,
		(int)chars /* size in characters, not bytes! */);
}

#endif


#ifdef _WIN32

bool
fsu_rmfile(char const *in_path)
{
	size_t nchars = sys_wchar_from_utf8(in_path, NULL, 0);
	assert(nchars > 0);
	wchar_t *utf16 = malloc(nchars * sizeof *utf16);
	sys_wchar_from_utf8(in_path, utf16, nchars);
	bool result = (DeleteFileW(utf16) == TRUE);
	free(utf16);
	return result;
}
#else

bool
fsu_rmfile(char const *in_path)
{
	return (unlink(in_path) == 0);
}
#endif


#ifdef _WIN32
static bool
fsu_recursive_mkdir(wchar_t *in_dir)
{
	// store the first directory to be created, for early outs
	wchar_t *first_hit = NULL;

	// go back, creating directories until one already exists or can be created
	wchar_t *end = in_dir + wcslen(in_dir);
	wchar_t *ptr = end;
	while (ptr >= in_dir)
	{
		if (*ptr == '\\' || *ptr == '/')
		{
			if (!first_hit)
			{
				first_hit = ptr;
			}
			wchar_t old = *ptr;
			*ptr = '\0';
			BOOL result = CreateDirectory(in_dir, NULL);
			DWORD err = GetLastError();
			printf("<CreateDirectory(%ls) %lu\n", in_dir, err);
			*ptr = old;

			if (result || err == ERROR_ALREADY_EXISTS)
			{
				printf("- done\n");
				break;
			}
		}
		--ptr;
	}

	// requested directory was already found or created
	if (ptr == first_hit)
	{
		return true;
	}

	// unable to create any of the required directories
	if (ptr < in_dir)
	{
		return false;
	}

	// now go back forward until the full directory is created
	while (ptr <= first_hit)
	{
		if (*ptr == '\\' || *ptr == '/')
		{
			wchar_t old = *ptr;
			*ptr = '\0';
			BOOL result = CreateDirectory(in_dir, NULL);
			DWORD err = GetLastError();
			printf(">CreateDirectory(%ls) %lu\n", in_dir, err);
			*ptr = old;

			if (result || err == ERROR_ALREADY_EXISTS)
			{
				printf("- done\n");
				if (ptr == end)
				{
					printf("- final\n");
					return true;
				}
				++ptr;
				continue;
			}
		}
		++ptr;
	}

	return false;
}


bool
fsu_mkdir(char const *in_path)
{
	size_t nchars = sys_wchar_from_utf8(in_path, NULL, 0);
	assert(nchars > 0);
	wchar_t *utf16 = malloc(nchars * sizeof *utf16);
	sys_wchar_from_utf8(in_path, utf16, nchars);
	bool result = fsu_recursive_mkdir(utf16);
	free(utf16);
	return result;
}

#else

bool
fsu_mkdir(char const *in_dir)
{
	assert(in_dir);

	// check if directory already exists
	struct stat sbuffer;
	if (stat(in_dir, &sbuffer) == 0)
	{
		return true;
	}

	char *dir = strdup(in_dir);
	char *ptr = dir;
	while (*(++ptr))
	{
		if (*ptr == '/')
		{
			*ptr = '\0';
			if (mkdir(dir, 0777 /* octal mode */) == -1 && errno != EEXIST)
			{
				free(dir);
				return false;
			}
			*ptr = '/';
		}
	}
	free(dir);
	return true;
}
#endif


#ifdef _WIN32
FILE *
fsu_fopen(char const *in_path, char const *in_mode)
{
	assert(in_mode);

	// convert to utf16
	size_t nchars = sys_wchar_from_utf8(in_path, NULL, 0);
	assert(nchars > 0);
	wchar_t *utf16 = malloc(nchars * sizeof *utf16);
	sys_wchar_from_utf8(in_path, utf16, nchars);

	bool has_write = false;
	// convert mode
	wchar_t wmode[8] = { 0 };
	for (size_t i = 0; i < 8 && in_mode[i]; ++i)
	{
		wmode[i] = (unsigned char)in_mode[i];
		if (in_mode[i] == 'w')
		{
			has_write = true;
		}
	}

	// create directory if mode contains 'w'
	if (has_write)
	{
		fsu_mkdir(in_path);
	}

	FILE *f = _wfopen(utf16, wmode);
	free(utf16);
	return f;
}
#else
FILE *
fsu_fopen(char const *path, char const *mode)
{
	// create directory if mode contains 'w'
	if (strchr(mode, 'w'))
	{
		fsu_mkdir(path);
	}
	return fopen(path, mode);
}
#endif


#ifdef _WIN32

bool
fsu_mvfile(char const *in_srcpath, char const *in_dstpath, bool in_replace)
{
	// copy allowed: don't care when destination is on another volume
	// write through: make sure the move is finished before proceeding
	//                'security' over speed
	DWORD flags = MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH;
	if (in_replace)
	{
		flags |= MOVEFILE_REPLACE_EXISTING;
	}

	// convert in_srcpath to utf16
	size_t nchars = sys_wchar_from_utf8(in_srcpath, NULL, 0);
	assert(nchars > 0);
	wchar_t *srcpath = malloc(nchars * sizeof *srcpath);
	sys_wchar_from_utf8(in_srcpath, srcpath, nchars);

	// convert in_dstpath to utf16
	nchars = sys_wchar_from_utf8(in_dstpath, NULL, 0);
	assert(nchars > 0);
	wchar_t *dstpath = malloc(nchars * sizeof *dstpath);
	sys_wchar_from_utf8(in_dstpath, dstpath, nchars);

	BOOL result = MoveFileExW(srcpath, dstpath, flags);
	if (!result)
	{
		printf(
			"[util] MoveFileEx#1 failed %lu (%lx)\n",
			GetLastError(),
			GetLastError());
	}
	if (result == FALSE && GetLastError() == ERROR_PATH_NOT_FOUND)
	{
		fsu_recursive_mkdir(dstpath);
		result = MoveFileExW(srcpath, dstpath, flags);
		if (!result)
		{
			printf(
				"[util] MoveFileEx#2 failed %lu (%lx)\n",
				GetLastError(),
				GetLastError());
		}
	}

	free(srcpath);
	free(dstpath);

	return (result == TRUE);
}

#else

static bool
fsu_cpfile(char const *in_srcpath, char const *in_dstpath, bool in_replace)
{
	// fail if something does exist at the destination but in_replace is false
	struct stat st = { 0 };
	if (!in_replace && stat(in_dstpath, &st) == 0)
	{
		return false;
	}

	// if the file cannot be opened, all bets are off.
	FILE *src = fopen(in_srcpath, "rb");
	if (!src)
	{
		return false;
	}

	// make sure the destination directory exists.
	fsu_mkdir(in_dstpath);
	// either the file does not exist, or in_replace is true so make sure
	// the old file does not exist anymore.
	unlink(in_dstpath);

	FILE *dst = fopen(in_dstpath, "wb");
	if (dst)
	{
		char buffer[4096];
		size_t nbytes = 0;
		while ((nbytes = fread(buffer, 1, sizeof buffer, src)) > 0)
		{
			fwrite(buffer, 1, nbytes, dst);
		}
		fclose(dst);
	}
	fclose(src);

	return (dst);
}


bool
fsu_mvfile(char const *in_srcpath, char const *in_dstpath, bool in_replace)
{
	fsu_mkdir(in_dstpath);

	// fail if something does exist at the destination but in_replace is false
	struct stat st = { 0 };
	if (!in_replace && stat(in_dstpath, &st) == 0)
	{
		return false;
	}

	int rv = rename(in_srcpath, in_dstpath);

	if (rv == 0)
	{
		return true;
	}
	else if (rv == -1 && errno == EXDEV)
	{
		// if rename() failed because src and dst were on different
		// file systems use mlfs_cpfile() and mlfs_rmfile()
		if (fsu_cpfile(in_srcpath, in_dstpath, in_replace))
		{
			fsu_rmfile(in_srcpath);
			return true;
		}
	}

	return false;
}

#endif


#ifdef _WIN32

int64_t
fsu_fsize(char const *in_path)
{
	// convert to utf16
	size_t nchars = sys_wchar_from_utf8(in_path, NULL, 0);
	assert(nchars);
	wchar_t *utf16 = malloc(nchars * sizeof *utf16);
	sys_wchar_from_utf8(in_path, utf16, nchars);

	HANDLE file = CreateFile(
		utf16,
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	free(utf16);

	// early out on failure
	if (file == INVALID_HANDLE_VALUE)
	{
		return -1;
	}

	LARGE_INTEGER size = { .QuadPart = 0 };
	GetFileSizeEx(file, &size);
	CloseHandle(file);
	return size.QuadPart;
}

#else

int64_t
fsu_fsize(char const *in_path)
{
	struct stat st;
	bool ok = (0 == stat(in_path, &st) && S_ISREG(st.st_mode));
	return ok ? st.st_size : -1;
}

#endif


#ifdef _WIN32

void
sys_sleep(uint32_t ms)
{
	Sleep(ms);
}

#else

void
sys_sleep(uint32_t ms)
{
	usleep(ms * 1000);
}

#endif


#ifdef _WIN32

int
asprintf(char **strp, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	int size = vasprintf(strp, fmt, args);

	va_end(args);

	return size;
}


int
vasprintf(char **strp, const char *fmt, va_list args)
{
	va_list tmpa;
	va_copy(tmpa, args);

	int size = vsnprintf(NULL, 0, fmt, tmpa);

	va_end(tmpa);

	if (size < 0)
	{
		return -1;
	}

	*strp = malloc((size_t)size + 1 /*NUL*/);
	if (NULL == *strp)
	{
		return -1;
	}

	return vsprintf(*strp, fmt, args);
}

#endif
