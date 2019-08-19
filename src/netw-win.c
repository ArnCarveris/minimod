// vi: noexpandtab tabstop=4 softtabstop=4 shiftwidth=0 list
#pragma comment(lib, "winhttp.lib")

#include "netw.h"
#include "util.h"

#include <stdio.h>
#include <windows.h>
#include <winhttp.h>


struct netw
{
	HINTERNET session;
	struct netw_callbacks callbacks;
};
static struct netw l_netw;


bool
netw_init(struct netw_callbacks *in_callbacks)
{
	l_netw.callbacks = *in_callbacks;

	l_netw.session = WinHttpOpen(
		L"minimod-client",

// TODO if windows < 8.1
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
// TODO if windows >= 8.1
		//WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,

		WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS,

		0);

	if (!l_netw.session)
	{
		printf("[netw] HttpOpen failed %lu (%lx)\n", GetLastError(), GetLastError());
		return false;
	}

	return true;
}


void
netw_deinit(void)
{
	WinHttpCloseHandle(l_netw.session);
}


static char *
combine_headers(char const *const in_headers[], size_t *out_len)
{
	size_t len_headers = 0;
	char const *const *h = in_headers;
	while (*h)
	{
		len_headers += strlen(*(h++));
		len_headers += 2; /* ": " or "\r\n" */
	}
	char *headers = malloc(len_headers + 1 /*NUL*/);

	h = in_headers;
	char *hdrptr = headers;
	while (*h)
	{
		// key
		size_t l = strlen(*h);
		memcpy(hdrptr, *h, l);
		hdrptr += l;
		*(hdrptr++) = ':';
		*(hdrptr++) = ' ';
		++h;

		// value
		l = strlen(*h);
		memcpy(hdrptr, *h, l);
		hdrptr += l;
		*(hdrptr++) = '\r';
		*(hdrptr++) = '\n';
		++h;
	}

	// NUL terminate
	*hdrptr = '\0';

	if (out_len)
	{
		*out_len = len_headers;
	}

	return headers;
}


static wchar_t*
wcstrndup(wchar_t const *in, size_t in_len)
{
	wchar_t *ptr = malloc(sizeof *ptr * (in_len + 1));
	ptr[in_len] = '\0';
	memcpy(ptr, in, sizeof *ptr * in_len);
	return ptr;
}


static void *
memdup(void const *in, size_t bytes)
{
	void *dup = malloc(bytes);
	memcpy(dup, in, bytes);
	return dup;
}


struct task
{
	wchar_t *host;
	wchar_t *path;
	wchar_t *header;
	void *udata;
	void *payload;
	size_t payload_bytes;
	uint16_t port;
};


static DWORD
data_task(LPVOID context)
{
	struct task *task = context;

	HINTERNET hconnection =
		WinHttpConnect(l_netw.session, task->host, task->port, 0);
	if (!hconnection)
	{
		printf("[netw] ERR: HttpConnect %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}

	HINTERNET hrequest = WinHttpOpenRequest(
		hconnection,
		L"GET",
		task->path,
		NULL,
		WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		WINHTTP_FLAG_SECURE);
	if (!hrequest)
	{
		printf("[netw] ERR: HttpOpenRequest: %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}

	printf("[netw] Sending request...\n");
	BOOL ok = WinHttpSendRequest(
		hrequest,
		task->header,
		(DWORD)-1,
		WINHTTP_NO_REQUEST_DATA,
		0,
		0,
		1);
	if (!ok)
	{
		printf("[netw] ERR: HttpSendRequest: %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}

	printf("[netw] Waiting for response...\n");
	ok = WinHttpReceiveResponse(hrequest, NULL);
	if (!ok)
	{
		printf("[netw] ERR: HttpReceiveResponse: %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}

	printf("[netw] Query headers...\n");
	DWORD status_code;
	DWORD sc_bytes = sizeof status_code;
	ok = WinHttpQueryHeaders(
		hrequest,
		WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX,
		&status_code,
		&sc_bytes,
		WINHTTP_NO_HEADER_INDEX);
	if (!ok)
	{
		printf("[netw] ERR: HttpQueryHeaders: %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}
	printf("[netw] status code of response: %lu\n", status_code);

	printf("[netw] Read content...\n");
	uint8_t *buffer = NULL;
	size_t bytes = 0;
	DWORD avail_bytes = 0;
	do
	{
		ok = WinHttpQueryDataAvailable(hrequest, &avail_bytes);
		if (!ok)
		{
			printf("[netw] ERR: QueryDataAvailable: %lu (0x%lx)\n", GetLastError(), GetLastError());
			return false;
		}
		if (avail_bytes > 0)
		{
			buffer = realloc(buffer, bytes + avail_bytes);
			DWORD actual_bytes = 0;
			WinHttpReadData(hrequest, buffer + bytes, avail_bytes, &actual_bytes);
			bytes += actual_bytes;
			printf("[netw] Read %lu from %lu bytes -> %zu bytes\n", actual_bytes, avail_bytes, bytes);
		}
	}
	while (avail_bytes > 0);

	l_netw.callbacks.completion(task->udata, buffer, bytes, (int)status_code);

	// free local data
	free(buffer);
	WinHttpCloseHandle(hrequest);
	WinHttpCloseHandle(hconnection);

	// free task data
	free(task->host);
	free(task->path);
	free(task->header);

	// free actual task
	free(task);

	return true;
}

bool
netw_get_request(char const *in_uri, char const *const in_headers[], void *udata)
{
	printf("[netw] get_request: %s\n", in_uri);

	struct task *task = malloc(sizeof *task);
	task->udata = udata;

	// convert/extract URI information
	size_t urilen = sys_wchar_from_utf8(in_uri, NULL, 0);
	wchar_t *uri = malloc(sizeof *uri * urilen);
	sys_wchar_from_utf8(in_uri, uri, urilen);

	URL_COMPONENTS url_components = {
		.dwStructSize = sizeof url_components,
		.dwHostNameLength = (DWORD)-1,
		.dwUrlPathLength = (DWORD)-1,
	};
	WinHttpCrackUrl(uri, (DWORD)urilen, 0, &url_components);

	task->port = url_components.nPort;
	wprintf(L"[netw] port: %i\n", task->port);
	task->host = wcstrndup(url_components.lpszHostName, url_components.dwHostNameLength);
	wprintf(L"[netw] host: %s\n", task->host);
	task->path = wcstrndup(url_components.lpszUrlPath, url_components.dwUrlPathLength);
	wprintf(L"[netw] path: %s\n", task->path);

	free(uri);

	// combine/convert headers
	char *header = combine_headers(in_headers, NULL);

	size_t headerlen = sys_wchar_from_utf8(header, NULL, 0);
	task->header = malloc(sizeof *(task->header) * headerlen);
	sys_wchar_from_utf8(header, task->header, headerlen);

	wprintf(L"[netw] headers:\n--\n%s--\n", task->header);

	free(header);

	HANDLE h = CreateThread(NULL, 0, data_task, task, 0, NULL);
	if (h)
	{
		CloseHandle(h);
	}
	else
	{
		printf("[netw] failed to create thread\n");
	}

	return true;
}


static DWORD
post_task(LPVOID context)
{
	struct task *task = context;

	HINTERNET hconnection =
		WinHttpConnect(l_netw.session, task->host, task->port, 0);
	if (!hconnection)
	{
		printf("[netw] ERR: HttpConnect %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}

	HINTERNET hrequest = WinHttpOpenRequest(
		hconnection,
		L"POST",
		task->path,
		NULL,
		WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		WINHTTP_FLAG_SECURE);
	if (!hrequest)
	{
		printf("[netw] ERR: HttpOpenRequest: %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}

	printf("[netw] Sending request...\n");
	printf("[netw] payload-bytes: %zu\n", task->payload_bytes);
	printf("[netw] payload: %s\n", task->payload);
	BOOL ok = WinHttpSendRequest(
		hrequest,
		task->header,
		(DWORD)-1,
		task->payload,
		(DWORD)task->payload_bytes,
		(DWORD)task->payload_bytes,
		1);
	if (!ok)
	{
		printf("[netw] ERR: HttpSendRequest: %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}

	printf("[netw] Waiting for response...\n");
	ok = WinHttpReceiveResponse(hrequest, NULL);
	if (!ok)
	{
		printf("[netw] ERR: HttpReceiveResponse: %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}

	printf("[netw] Query headers...\n");
	DWORD status_code;
	DWORD sc_bytes = sizeof status_code;
	ok = WinHttpQueryHeaders(
		hrequest,
		WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX,
		&status_code,
		&sc_bytes,
		WINHTTP_NO_HEADER_INDEX);
	if (!ok)
	{
		printf("[netw] ERR: HttpQueryHeaders: %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}
	printf("[netw] status code of response: %lu\n", status_code);

	printf("[netw] Read content...\n");
	uint8_t *buffer = NULL;
	size_t bytes = 0;
	DWORD avail_bytes = 0;
	do
	{
		ok = WinHttpQueryDataAvailable(hrequest, &avail_bytes);
		if (!ok)
		{
			printf("[netw] ERR: QueryDataAvailable: %lu (0x%lx)\n", GetLastError(), GetLastError());
			return false;
		}
		if (avail_bytes > 0)
		{
			buffer = realloc(buffer, bytes + avail_bytes);
			DWORD actual_bytes = 0;
			WinHttpReadData(hrequest, buffer + bytes, avail_bytes, &actual_bytes);
			bytes += actual_bytes;
			printf("[netw] Read %lu from %lu bytes -> %zu bytes\n", actual_bytes, avail_bytes, bytes);
		}
	}
	while (avail_bytes > 0);

	l_netw.callbacks.completion(task->udata, buffer, bytes, (int)status_code);

	// free local data
	free(buffer);
	WinHttpCloseHandle(hrequest);
	WinHttpCloseHandle(hconnection);

	// free task data
	free(task->host);
	free(task->path);
	free(task->header);
	free(task->payload);

	// free actual task
	free(task);

	return true;
}


bool
netw_post_request(
	char const *in_uri,
	char const *const in_headers[],
	void const *body,
	size_t nbody_bytes,
	void *udata)
{
	printf("[netw] post_request: %s\n", in_uri);

	struct task *task = malloc(sizeof *task);
	task->udata = udata;
	task->payload = memdup(body, nbody_bytes);
	task->payload_bytes = nbody_bytes;

	// convert/extract URI information
	size_t urilen = sys_wchar_from_utf8(in_uri, NULL, 0);
	wchar_t *uri = malloc(sizeof *uri * urilen);
	sys_wchar_from_utf8(in_uri, uri, urilen);

	URL_COMPONENTS url_components = {
		.dwStructSize = sizeof url_components,
		.dwHostNameLength = (DWORD)-1,
		.dwUrlPathLength = (DWORD)-1,
	};
	WinHttpCrackUrl(uri, (DWORD)urilen, 0, &url_components);

	task->port = url_components.nPort;
	wprintf(L"[netw] port: %i\n", task->port);
	task->host = wcstrndup(url_components.lpszHostName, url_components.dwHostNameLength);
	wprintf(L"[netw] host: %s\n", task->host);
	task->path = wcstrndup(url_components.lpszUrlPath, url_components.dwUrlPathLength);
	wprintf(L"[netw] path: %s\n", task->path);

	free(uri);

	// combine/convert headers
	char *header = combine_headers(in_headers, NULL);

	size_t headerlen = sys_wchar_from_utf8(header, NULL, 0);
	task->header = malloc(sizeof *(task->header) * headerlen);
	sys_wchar_from_utf8(header, task->header, headerlen);

	wprintf(L"[netw] headers:\n--\n%s--\n", task->header);

	free(header);

	HANDLE h = CreateThread(NULL, 0, post_task, task, 0, NULL);
	if (h)
	{
		CloseHandle(h);
	}
	else
	{
		printf("[netw] failed to create thread\n");
	}

	return true;
}


static DWORD
download_task(LPVOID context)
{
	struct task *task = context;

	HINTERNET hconnection =
		WinHttpConnect(l_netw.session, task->host, task->port, 0);
	if (!hconnection)
	{
		printf("[netw] ERR: HttpConnect %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}

	HINTERNET hrequest = WinHttpOpenRequest(
		hconnection,
		L"POST",
		task->path,
		NULL,
		WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		WINHTTP_FLAG_SECURE);
	if (!hrequest)
	{
		printf("[netw] ERR: HttpOpenRequest: %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}

	printf("[netw] Sending request...\n");
	printf("[netw] payload-bytes: %zu\n", task->payload_bytes);
	printf("[netw] payload: %s\n", task->payload);
	BOOL ok = WinHttpSendRequest(
		hrequest,
		task->header,
		(DWORD)-1,
		task->payload,
		(DWORD)task->payload_bytes,
		(DWORD)task->payload_bytes,
		1);
	if (!ok)
	{
		printf("[netw] ERR: HttpSendRequest: %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}

	printf("[netw] Waiting for response...\n");
	ok = WinHttpReceiveResponse(hrequest, NULL);
	if (!ok)
	{
		printf("[netw] ERR: HttpReceiveResponse: %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}

	printf("[netw] Query headers...\n");
	DWORD status_code;
	DWORD sc_bytes = sizeof status_code;
	ok = WinHttpQueryHeaders(
		hrequest,
		WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX,
		&status_code,
		&sc_bytes,
		WINHTTP_NO_HEADER_INDEX);
	if (!ok)
	{
		printf("[netw] ERR: HttpQueryHeaders: %lu (0x%lx)\n", GetLastError(), GetLastError());
		return false;
	}
	printf("[netw] status code of response: %lu\n", status_code);

	printf("[netw] Setting up temporary file\n");

	// funny how GetTempPath() requires up to MAX_PATH+1 characters without
	// the terminating \0, while GetTempFileName(), which appends to this
	// very string requires its output-buffer to be MAX_PATH only (including
	// \0). And in fact GetTempFileName() fails with ERROR_BUFFER_OVERFLOW
	// when its first argument is > MAX_PATH-14.
	// Well done Microsoft.
	wchar_t temp_dir[MAX_PATH+1+1] = {0};
	GetTempPathW(MAX_PATH+1, temp_dir);
	wchar_t temp_path[MAX_PATH] = {0};
	GetTempFileName(temp_dir, L"mmi", 0, temp_path);
	wprintf(L"[netw] Setting up temporary file: %s\n", temp_path);
	HANDLE hfile = CreateFile(
		temp_path,
		GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_DELETE,
		NULL,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_TEMPORARY,
		NULL);
	if (!hfile)
	{
		printf("[netw] Failed to create temporary file\n");
		return false;
	}

	printf("[netw] Read content...\n");
#define BUFFERSIZE 4096
	uint8_t *buffer = malloc(BUFFERSIZE);
	DWORD avail_bytes = 0;
	do
	{
		ok = WinHttpQueryDataAvailable(hrequest, &avail_bytes);
		if (!ok)
		{
			printf("[netw] ERR: QueryDataAvailable: %lu (0x%lx)\n", GetLastError(), GetLastError());
			return false;
		}
		if (avail_bytes > 0)
		{
			DWORD actual_bytes_read = 0;
			WinHttpReadData(hrequest, buffer, BUFFERSIZE, &actual_bytes_read);
			printf("[netw] Read %lu from %lu bytes\n", actual_bytes_read, avail_bytes);
			DWORD actual_bytes_written = 0;
			do
			{
				WriteFile(hfile, buffer, actual_bytes_read, &actual_bytes_written, NULL);
				actual_bytes_read -= actual_bytes_written;
			} while (actual_bytes_read > 0);
			printf("[netw] Written %lu from %lu bytes\n", actual_bytes_written, actual_bytes_read);
		}
	}
	while (avail_bytes > 0);

	// convert path to utf8
	size_t pathlen = sys_utf8_from_wchar(temp_path, NULL, 0);
	char *u8path = malloc(pathlen);
	sys_utf8_from_wchar(temp_path, u8path, pathlen);

	l_netw.callbacks.downloaded(task->udata, u8path, (int)status_code);

	CloseHandle(hfile);
	DeleteFile(temp_path);

	free(u8path);

	// free local data
	free(buffer);
	WinHttpCloseHandle(hrequest);
	WinHttpCloseHandle(hconnection);

	// free task data
	free(task->host);
	free(task->path);
	free(task->header);
	free(task->payload);

	// free actual task
	free(task);

	return true;
}


bool
netw_download(char const *in_uri, void *udata)
{
	printf("[netw] download_request: %s\n", in_uri);

	struct task *task = calloc(sizeof *task, 1);
	task->udata = udata;

	// convert/extract URI information
	size_t urilen = sys_wchar_from_utf8(in_uri, NULL, 0);
	wchar_t *uri = malloc(sizeof *uri * urilen);
	sys_wchar_from_utf8(in_uri, uri, urilen);

	URL_COMPONENTS url_components = {
		.dwStructSize = sizeof url_components,
		.dwHostNameLength = (DWORD)-1,
		.dwUrlPathLength = (DWORD)-1,
	};
	WinHttpCrackUrl(uri, (DWORD)urilen, 0, &url_components);

	task->port = url_components.nPort;
	wprintf(L"[netw] port: %i\n", task->port);
	task->host = wcstrndup(url_components.lpszHostName, url_components.dwHostNameLength);
	wprintf(L"[netw] host: %s\n", task->host);
	task->path = wcstrndup(url_components.lpszUrlPath, url_components.dwUrlPathLength);
	wprintf(L"[netw] path: %s\n", task->path);

	free(uri);

	HANDLE h = CreateThread(NULL, 0, download_task, task, 0, NULL);
	if (h)
	{
		CloseHandle(h);
	}
	else
	{
		printf("[netw] failed to create thread\n");
	}

	return true;
}
