﻿/*
 * Copyright (C) 2019, 2021, Yasumasa Suenaga
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>

#include <iostream>

#include "SerialSetup.h"
#include "WinAPIException.h"


#define RW_BUFFER_SIZE	2048

static HANDLE stdoutRedirectorThread;

static HANDLE hSerial;
static OVERLAPPED serialReadOverlapped = { 0 };
static volatile bool terminated;
static volatile bool pause = false;
static TString Port;


#ifdef _UNICODE
static void ErrorMessage(HWND hwnd, LPCWSTR lpText, LPCWSTR lpCaption)
#else
static void ErrorMessage(HWND hwnd, LPCSTR lpText, LPCSTR lpCaption)
#endif
{
#ifdef CONSOLE_ONLY
    std::cout << lpText << std::endl;
#else
    MessageBox(hwnd, lpText, lpCaption, MB_OK | MB_ICONERROR);
#endif
}

static BOOL IsCharMatch(TCHAR *chars, int len, const char *value)
{
	int vlen = strlen(value);

	if (len != vlen)
		return FALSE;

	for (int i = 0; i < len; i++)
	{
		if (chars[i] != value[i])
			return FALSE;
	}
	return TRUE;
}

static int EscapeChar(TCHAR *chars, int len)
{
	const char *f1 = "OP";
	const char *f8 = "[19~";

	if (len < 2 || chars[0] != 0x1B)
		return 0;

	if (IsCharMatch(chars + 1, len - 1, f1))
		return 1;
	if (IsCharMatch(chars + 1, len - 1, f8))
		return 8;

	return 0;
}

/*
 * Entry point for stdin redirector.
 * stdin redirects stdin to serial (write op).
 */
static void StdInRedirector(HWND parent_hwnd) {
	HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
	OVERLAPPED overlapped = { 0 };
	TCHAR console_data[RW_BUFFER_SIZE];
	char send_data[RW_BUFFER_SIZE];
	DWORD data_len;

	overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (overlapped.hEvent == NULL) {
		WinAPIException ex(GetLastError(), _T("SimpleCom"));
		ErrorMessage(parent_hwnd, ex.GetErrorText(), ex.GetErrorCaption());
		return;
	}

	while (!terminated) {
		ReadConsole(hStdIn, console_data, RW_BUFFER_SIZE, &data_len, nullptr);

		if (EscapeChar(console_data, data_len) == 1) {	// F1 key
#ifdef CONSOLE_ONLY
            terminated = true;
            break;
#else
			if (MessageBox(parent_hwnd, _T("Do you want to leave from this serial session?"), _T("SimpleCom"), MB_YESNO | MB_ICONQUESTION) == IDYES) {
                terminated = true;
                break;
			}
			else {
				continue;
			}
#endif // CONSOLE_ONLY
		}
		else if (EscapeChar(console_data, data_len) == 8){ 	// F8 key
		    pause = !pause;

		    TString title = _T("SimpleCom: ") + Port;
		    if (pause)
                title += _T(" [PAUSE]");
		    SetConsoleTitle(title.c_str());
		}
		else {
			// ReadConsole() is called as ANS mode (pInputControl is NULL)
			for (DWORD i = 0; i < data_len; i++) {
				send_data[i] = console_data[i] & 0xff;
			}
		}

		if (!pause) {
			ResetEvent(overlapped.hEvent);
			DWORD nBytesWritten;
			if (!WriteFile(hSerial, send_data, data_len, &nBytesWritten, &overlapped)) {
				if (GetLastError() == ERROR_IO_PENDING) {
					if (!GetOverlappedResult(hSerial, &overlapped, &nBytesWritten, TRUE)) {
						break;
					}
				}
			}
		}
	}

	CloseHandle(overlapped.hEvent);
}

/*
 * Entry point for stdout redirector.
 * stdout redirects serial (read op) to stdout.
 */
DWORD WINAPI StdOutRedirector(_In_ LPVOID lpParameter) {
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	char buf;
	DWORD nBytesRead;

	while (!terminated) {
		ResetEvent(serialReadOverlapped.hEvent);
		if (!ReadFile(hSerial, &buf, 1, &nBytesRead, &serialReadOverlapped)) {
			if (GetLastError() == ERROR_IO_PENDING) {
				if (!GetOverlappedResult(hSerial, &serialReadOverlapped, &nBytesRead, TRUE)) {
					break;
				}
			}
		}

		if (nBytesRead > 0 && !pause) {
			DWORD nBytesWritten;
			WriteFile(hStdOut, &buf, nBytesRead, &nBytesWritten, NULL);
		}
	}

	return 0;
}

static HWND GetParentWindow() {
	HWND current = GetConsoleWindow();

	while (true) {
		HWND parent = GetParent(current);

		if (parent == NULL) {
			TCHAR window_text[MAX_PATH] = { 0 };
			GetWindowText(current, window_text, MAX_PATH);

			// If window_text is empty, SimpleCom might be run on Windows Terminal (Could not get valid owner window text)
			return (window_text[0] == 0) ? NULL : current;
		}
		else {
			current = parent;
		}

	}

}

int _tmain(int argc, LPCTSTR argv[])
{
	DCB dcb;
	TString device;
	HWND parent_hwnd = GetParentWindow();

	// Serial port configuration
	try {
		SerialSetup setup;
		if (argc > 1) {
			// command line mode
			setup.ParseArguments(argc, argv);
		}
#ifndef CONSOLE_ONLY
		else if (!setup.ShowConfigureDialog(NULL, parent_hwnd)) {
			return -1;
		}
#endif
        Port = setup.GetPort();
		device = _T("\\\\.\\") + Port;
		setup.SaveToDCB(&dcb);
	}
	catch (WinAPIException &e) {
	    ErrorMessage(parent_hwnd, e.GetErrorText(), e.GetErrorCaption());
		return -1;
	}
	catch (SerialSetupException &e) {
		ErrorMessage(parent_hwnd, e.GetErrorText(), e.GetErrorCaption());
		return -2;
	}

	// Open serial device
	hSerial = CreateFile(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (hSerial == INVALID_HANDLE_VALUE) {
		WinAPIException e(GetLastError(), NULL); // Use WinAPIException to get error string.

		TString error = _T("Open Serial ") + Port + _T(" Fail, Reason: ") + e.GetErrorText();
		ErrorMessage(parent_hwnd, error.c_str(), _T("Open serial connection"));
		return -4;
	}

	TString title = _T("SimpleCom: ") + Port;
	SetConsoleTitle(title.c_str());

	SetCommState(hSerial, &dcb);
	PurgeComm(hSerial, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR);
	SetCommMask(hSerial, EV_RXCHAR);
	SetupComm(hSerial, 1, 1);

	serialReadOverlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (serialReadOverlapped.hEvent == NULL) {
		WinAPIException ex(GetLastError(), _T("SimpleCom"));
		ErrorMessage(parent_hwnd, ex.GetErrorText(), ex.GetErrorCaption());
		CloseHandle(hSerial);
		return -1;
	}

	DWORD mode;

	HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
	GetConsoleMode(hStdIn, &mode);
	mode &= ~ENABLE_PROCESSED_INPUT;
	mode &= ~ENABLE_LINE_INPUT;
	mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
	SetConsoleMode(hStdIn, mode);

	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	GetConsoleMode(hStdOut, &mode);
	mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
	SetConsoleMode(hStdOut, mode);

	terminated = false;

	// Create stdout redirector
	stdoutRedirectorThread = CreateThread(NULL, 0, &StdOutRedirector, NULL, 0, NULL);
	if (stdoutRedirectorThread == NULL) {
		WinAPIException ex(GetLastError(), _T("SimpleCom"));
		ErrorMessage(parent_hwnd, ex.GetErrorText(), ex.GetErrorCaption());
		CloseHandle(hSerial);
		return -2;
	}

	// stdin redirector would perform in current thread
	StdInRedirector(parent_hwnd);

	// stdin redirector should be finished at this point.
	// It means end of serial communication. So we should terminate stdout redirector.
	CancelIoEx(hSerial, &serialReadOverlapped);
	WaitForSingleObject(stdoutRedirectorThread, INFINITE);

	CloseHandle(hSerial);
	CloseHandle(serialReadOverlapped.hEvent);

	return 0;
}
