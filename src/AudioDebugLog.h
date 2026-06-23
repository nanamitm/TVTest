#pragma once

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <string>

// Callers are expected to have already included "AppMain.h" for GetAppClass().

namespace {

// Only logs when the running TVTest.ini has [Debug] AudioLogPath= set. When
// absent, the cached path stays empty and every call after the first
// returns immediately, so normal use has no overhead and writes no file.
inline const std::wstring &GetAudioDebugLogPath()
{
	static const std::wstring s_Path = [] {
		wchar_t LogPath[MAX_PATH] = {};
		::GetPrivateProfileStringW(
			L"Debug", L"AudioLogPath", L"",
			LogPath, MAX_PATH, TVTest::GetAppClass().GetIniFileName());
		return std::wstring(LogPath);
	}();

	return s_Path;
}

}

inline void AudioDebugLog(const wchar_t *pFormat, ...)
{
	const std::wstring &LogPath = GetAudioDebugLogPath();
	if (LogPath.empty())
		return;

	FILE *fp = nullptr;
	if (_wfopen_s(&fp, LogPath.c_str(), L"a, ccs=UTF-8") != 0 || fp == nullptr)
		return;

	SYSTEMTIME st;
	::GetLocalTime(&st);
	fwprintf(fp, L"%02d:%02d:%02d.%03d [tid=%04x] ",
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ::GetCurrentThreadId());

	va_list args;
	va_start(args, pFormat);
	vfwprintf(fp, pFormat, args);
	va_end(args);

	fwprintf(fp, L"\n");
	fclose(fp);
}
