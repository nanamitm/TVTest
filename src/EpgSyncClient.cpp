/*
  TVTest
  Copyright(c) 2008-2020 DBCTRADO

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/


#include "stdafx.h"
#include "TVTest.h"
#include "AppMain.h"
#include "EpgSyncClient.h"
#include "StringFormat.h"
#include "LibISDB/LibISDB/Base/MemoryStream.hpp"
#include "LibISDB/LibISDB/EPG/EPGDataSerializer.hpp"
#include <winhttp.h>
#include <process.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include "Common/DebugDef.h"

#pragma comment(lib, "winhttp.lib")


namespace TVTest
{

namespace
{


constexpr DWORD HTTP_TIMEOUT = 15 * 1000;
constexpr DWORD STREAM_TIMEOUT = 60 * 1000;	// SSE のキープアライブ(20 秒)より長く
constexpr DWORD RECONNECT_INTERVAL_MIN = 5 * 1000;
constexpr DWORD RECONNECT_INTERVAL_MAX = 60 * 1000;
constexpr int PUT_RETRY_COUNT = 3;
constexpr size_t MAX_RESPONSE_SIZE = 64 * 1024 * 1024;


String FromUTF8(const char *pData, size_t Size)
{
	if ((pData == nullptr) || (Size == 0))
		return String();

	const int Length = ::MultiByteToWideChar(
		CP_UTF8, 0, pData, static_cast<int>(Size), nullptr, 0);
	if (Length <= 0)
		return String();

	String Result(Length, L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, pData, static_cast<int>(Size), Result.data(), Length);

	return Result;
}


std::string ToUTF8(LPCTSTR pszText)
{
	if (IsStringEmpty(pszText))
		return std::string();

	const int SrcLength = ::lstrlen(pszText);
	const int Length = ::WideCharToMultiByte(
		CP_UTF8, 0, pszText, SrcLength, nullptr, 0, nullptr, nullptr);
	if (Length <= 0)
		return std::string();

	std::string Result(Length, '\0');
	::WideCharToMultiByte(
		CP_UTF8, 0, pszText, SrcLength, Result.data(), Length, nullptr, nullptr);

	return Result;
}


std::string EscapeJSONString(LPCTSTR pszText)
{
	const std::string Src = ToUTF8(pszText);
	std::string Result;

	Result.reserve(Src.length() + 8);
	for (const unsigned char c : Src) {
		switch (c) {
		case '"': Result += "\\\""; break;
		case '\\': Result += "\\\\"; break;
		case '\b': Result += "\\b"; break;
		case '\f': Result += "\\f"; break;
		case '\n': Result += "\\n"; break;
		case '\r': Result += "\\r"; break;
		case '\t': Result += "\\t"; break;
		default:
			if (c < 0x20) {
				char szCode[7];
				::StringCchPrintfA(szCode, std::size(szCode), "\\u%04X", c);
				Result += szCode;
			} else {
				Result.push_back(static_cast<char>(c));
			}
			break;
		}
	}

	return Result;
}


const char *GetNetworkTypeName(WORD NetworkID)
{
	// スカパー!プレミアムサービスは通常の110度CSとは分けて表示する。
	if (NetworkID == 0x000A)
		return "other";

	switch (GetAppClass().NetworkDefinition.GetNetworkType(NetworkID)) {
	case CNetworkDefinition::NetworkType::Terrestrial: return "terrestrial";
	case CNetworkDefinition::NetworkType::BS:          return "bs";
	case CNetworkDefinition::NetworkType::CS:          return "cs";
	default:                                           return "other";
	}
}


/** 番組表に出すロゴ

CDT から受信したままの ARIB 形式 PNG を持つ。ブラウザが読める PNG への
変換はサーバ側で行う。
*/
struct LogoImage
{
	WORD NetworkID = 0;
	WORD LogoID = 0;
	WORD LogoVersion = 0;
	BYTE LogoType = 0;
	std::vector<BYTE> Data;
};


/** ロゴの種別を選ぶ優先順

高度BS(NID 0x000B)は 64x36 (種別 5) しか送られてこないため、これを最優先に
する。以降は大きいものから順に選ぶ。
*/
constexpr BYTE LOGO_TYPE_PREFERENCE[] = {0x05, 0x03, 0x04, 0x02, 0x00, 0x01};


/** サービスに対応するロゴの種別を選ぶ。無ければ false */
bool SelectLogoType(WORD NetworkID, WORD ServiceID, BYTE *pLogoType)
{
	const DWORD Available = GetAppClass().LogoManager.GetAvailableLogoType(NetworkID, ServiceID);

	for (const BYTE Type : LOGO_TYPE_PREFERENCE) {
		if (Available & (1UL << Type)) {
			*pLogoType = Type;
			return true;
		}
	}

	return false;
}


std::string BuildServiceMetadata(std::vector<LogoImage> *pLogos)
{
	const CChannelManager &ChannelManager = GetAppClass().ChannelManager;
	const CChannelList *pChannelList = ChannelManager.GetAllChannelList();
	CLogoManager &LogoManager = GetAppClass().LogoManager;
	std::string JSON = "{\"services\":[";
	std::set<unsigned long long> Seen;
	std::set<unsigned long long> LogoSeen;
	int Order = 0;

	if (pLogos != nullptr)
		pLogos->clear();

	if (pChannelList != nullptr) {
		for (int i = 0; i < pChannelList->NumChannels(); i++) {
			const CChannelInfo *pChannel = pChannelList->GetChannelInfo(i);
			// 無効なチャンネルも局名の取得元にする。
			// サブチャンネルやワンセグはチャンネルリスト上で無効になっている
			// ことが多いが、EPG は EIT から取得したものをすべて送るため、
			// ここで除外すると番組表に局名を出せないサービスが生じる。
			if (pChannel == nullptr || IsStringEmpty(pChannel->GetName()))
				continue;

			const unsigned long long Key =
				(static_cast<unsigned long long>(pChannel->GetNetworkID()) << 32)
				| (static_cast<unsigned long long>(pChannel->GetTransportStreamID()) << 16)
				| pChannel->GetServiceID();
			if (!Seen.insert(Key).second)
				continue;

			if (Order > 0)
				JSON += ',';
			JSON += "{\"nid\":" + std::to_string(pChannel->GetNetworkID());
			JSON += ",\"tsid\":" + std::to_string(pChannel->GetTransportStreamID());
			JSON += ",\"sid\":" + std::to_string(pChannel->GetServiceID());
			JSON += ",\"name\":\"" + EscapeJSONString(pChannel->GetName()) + '"';
			JSON += ",\"group\":\"" + EscapeJSONString(
				ChannelManager.GetTuningSpaceName(pChannel->GetSpace())) + '"';
			JSON += ",\"network_type\":\"";
			JSON += GetNetworkTypeName(pChannel->GetNetworkID());
			JSON += '"';
			JSON += ",\"remote_control_key\":" + std::to_string(pChannel->GetChannelNo());
			JSON += ",\"service_type\":" + std::to_string(pChannel->GetServiceType());
			JSON += ",\"order\":" + std::to_string(Order);

			CLogoManager::LogoInfo LogoInfo;
			BYTE LogoType;
			if (SelectLogoType(pChannel->GetNetworkID(), pChannel->GetServiceID(), &LogoType)
					&& LogoManager.GetLogoInfo(
						pChannel->GetNetworkID(), pChannel->GetServiceID(), LogoType, &LogoInfo)) {
				JSON += ",\"logo_id\":" + std::to_string(LogoInfo.LogoID);
				JSON += ",\"logo_type\":" + std::to_string(LogoInfo.LogoType);
				JSON += ",\"logo_version\":" + std::to_string(LogoInfo.LogoVersion);

				// 同じロゴを複数のサービスが共有するので、実体は一度だけ集める。
				const unsigned long long LogoKey =
					(static_cast<unsigned long long>(LogoInfo.NetworkID) << 32)
					| (static_cast<unsigned long long>(LogoInfo.LogoID) << 16)
					| LogoInfo.LogoType;
				if ((pLogos != nullptr) && LogoSeen.insert(LogoKey).second) {
					LogoImage Logo;
					Logo.NetworkID = LogoInfo.NetworkID;
					Logo.LogoID = LogoInfo.LogoID;
					Logo.LogoVersion = LogoInfo.LogoVersion;
					Logo.LogoType = LogoInfo.LogoType;
					if (LogoManager.GetLogoData(
							Logo.NetworkID, Logo.LogoID, Logo.LogoType, &Logo.Data))
						pLogos->push_back(std::move(Logo));
				}
			}

			JSON += '}';
			Order++;
		}
	}

	JSON += "]}";

	return JSON;
}


/** URL を分解する */
struct UrlParts
{
	String Host;
	INTERNET_PORT Port = INTERNET_DEFAULT_HTTP_PORT;
	bool fSecure = false;
	String PathPrefix;
};


bool ParseUrl(const String &Url, UrlParts *pParts)
{
	String Normalized = Url;

	if (Normalized.find(L"://") == String::npos)
		Normalized = L"http://" + Normalized;

	URL_COMPONENTS Components = {};
	WCHAR szHost[256] = {};
	WCHAR szPath[1024] = {};

	Components.dwStructSize = sizeof(Components);
	Components.lpszHostName = szHost;
	Components.dwHostNameLength = static_cast<DWORD>(std::size(szHost));
	Components.lpszUrlPath = szPath;
	Components.dwUrlPathLength = static_cast<DWORD>(std::size(szPath));

	if (!::WinHttpCrackUrl(
			Normalized.c_str(), static_cast<DWORD>(Normalized.length()), 0, &Components))
		return false;

	pParts->Host = szHost;
	pParts->Port = Components.nPort;
	pParts->fSecure = (Components.nScheme == INTERNET_SCHEME_HTTPS);

	pParts->PathPrefix = szPath;
	// 末尾の '/' は各要求のパスと重複するので落とす
	while (!pParts->PathPrefix.empty() && (pParts->PathPrefix.back() == L'/'))
		pParts->PathPrefix.pop_back();

	return !pParts->Host.empty();
}


/** WinHTTP の薄いラッパ

要求は同期で行う。ストリーム(SSE)の読み込みは、別スレッドから
CancelStream() でハンドルを閉じることで中断できる。
*/
class CHttpClient
{
public:
	struct Response
	{
		DWORD StatusCode = 0;
		std::vector<BYTE> Body;
		String ETag;
		unsigned long long Version = 0;
	};

	~CHttpClient() { Close(); }

	bool Open(const UrlParts &Url)
	{
		Close();

		m_Url = Url;

		m_hSession = ::WinHttpOpen(
			L"TVTest EPG Sync/1.0",
			WINHTTP_ACCESS_TYPE_NO_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (m_hSession == nullptr)
			return false;

		::WinHttpSetTimeouts(m_hSession, HTTP_TIMEOUT, HTTP_TIMEOUT, HTTP_TIMEOUT, HTTP_TIMEOUT);

		m_hConnect = ::WinHttpConnect(m_hSession, m_Url.Host.c_str(), m_Url.Port, 0);
		if (m_hConnect == nullptr) {
			Close();
			return false;
		}

		return true;
	}

	void Close()
	{
		CancelStream();
		EndStream();

		if (m_hConnect != nullptr) {
			::WinHttpCloseHandle(m_hConnect);
			m_hConnect = nullptr;
		}
		if (m_hSession != nullptr) {
			::WinHttpCloseHandle(m_hSession);
			m_hSession = nullptr;
		}
	}

	bool IsOpen() const { return m_hConnect != nullptr; }

	bool Request(
		LPCWSTR pszMethod, const String &Path, const String &Headers,
		const void *pBody, size_t BodySize, Response *pResponse)
	{
		if (m_hConnect == nullptr)
			return false;

		const HINTERNET hRequest = OpenRequest(pszMethod, Path);
		if (hRequest == nullptr)
			return false;

		bool fOK = false;

		if (SendRequest(hRequest, Headers, pBody, BodySize)
				&& ::WinHttpReceiveResponse(hRequest, nullptr)) {
			pResponse->StatusCode = QueryStatusCode(hRequest);
			pResponse->ETag = QueryHeader(hRequest, L"ETag");
			pResponse->Version = StringToUInt64Safe(QueryHeader(hRequest, L"X-EPG-Version"));
			fOK = ReadAllData(hRequest, &pResponse->Body);
		}

		::WinHttpCloseHandle(hRequest);

		return fOK;
	}

	/** ストリームの読み込みを開始する */
	bool BeginStream(const String &Path, const String &Headers)
	{
		if (m_hConnect == nullptr)
			return false;

		EndStream();

		HINTERNET hRequest = OpenRequest(L"GET", Path);
		if (hRequest == nullptr)
			return false;

		// キープアライブの間隔より長く待てるようにする
		::WinHttpSetTimeouts(hRequest, HTTP_TIMEOUT, HTTP_TIMEOUT, HTTP_TIMEOUT, STREAM_TIMEOUT);

		if (!SendRequest(hRequest, Headers, nullptr, 0)
				|| !::WinHttpReceiveResponse(hRequest, nullptr)
				|| (QueryStatusCode(hRequest) != 200)) {
			::WinHttpCloseHandle(hRequest);
			return false;
		}

		std::lock_guard<std::mutex> Lock(m_StreamLock);
		m_hStream = hRequest;
		m_fStreamCanceled = false;

		return true;
	}

	bool ReadStream(void *pBuffer, DWORD Size, DWORD *pRead)
	{
		HINTERNET hStream;
		{
			std::lock_guard<std::mutex> Lock(m_StreamLock);
			if (m_fStreamCanceled)
				return false;
			hStream = m_hStream;
		}

		if (hStream == nullptr)
			return false;

		*pRead = 0;

		// WinHttpReadData() は同期モードでは要求したサイズが埋まるまで返らない。
		// SSE のように少しずつ届く応答では、到着済みのサイズを取ってからその分だけ読む。
		DWORD Available = 0;

		if (!::WinHttpQueryDataAvailable(hStream, &Available))
			return false;
		if (Available == 0)
			return false;	// 応答が終了した

		return ::WinHttpReadData(hStream, pBuffer, std::min(Available, Size), pRead)
			&& (*pRead > 0);
	}

	void EndStream()
	{
		HINTERNET hStream = nullptr;
		{
			std::lock_guard<std::mutex> Lock(m_StreamLock);
			std::swap(hStream, m_hStream);
		}
		if (hStream != nullptr)
			::WinHttpCloseHandle(hStream);
	}

	/** 別スレッドからストリームの読み込みを中断させる */
	void CancelStream()
	{
		std::lock_guard<std::mutex> Lock(m_StreamLock);

		m_fStreamCanceled = true;

		if (m_hStream != nullptr) {
			// 読み込み中の WinHttpReadData を失敗させる
			::WinHttpCloseHandle(m_hStream);
			m_hStream = nullptr;
		}
	}

private:
	HINTERNET OpenRequest(LPCWSTR pszMethod, const String &Path)
	{
		const String FullPath = m_Url.PathPrefix + Path;

		return ::WinHttpOpenRequest(
			m_hConnect, pszMethod, FullPath.c_str(),
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
			m_Url.fSecure ? WINHTTP_FLAG_SECURE : 0);
	}

	static bool SendRequest(
		HINTERNET hRequest, const String &Headers, const void *pBody, size_t BodySize)
	{
		return ::WinHttpSendRequest(
			hRequest,
			Headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : Headers.c_str(),
			Headers.empty() ? 0 : static_cast<DWORD>(Headers.length()),
			const_cast<void *>(pBody), static_cast<DWORD>(BodySize),
			static_cast<DWORD>(BodySize), 0) != FALSE;
	}

	static DWORD QueryStatusCode(HINTERNET hRequest)
	{
		DWORD StatusCode = 0;
		DWORD Size = sizeof(StatusCode);

		if (!::WinHttpQueryHeaders(
				hRequest,
				WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &StatusCode, &Size, WINHTTP_NO_HEADER_INDEX))
			return 0;

		return StatusCode;
	}

	static String QueryHeader(HINTERNET hRequest, LPCWSTR pszName)
	{
		DWORD Size = 0;

		::WinHttpQueryHeaders(
			hRequest, WINHTTP_QUERY_CUSTOM, pszName,
			WINHTTP_NO_OUTPUT_BUFFER, &Size, WINHTTP_NO_HEADER_INDEX);
		if ((Size == 0) || (::GetLastError() != ERROR_INSUFFICIENT_BUFFER))
			return String();

		String Value(Size / sizeof(WCHAR), L'\0');

		if (!::WinHttpQueryHeaders(
				hRequest, WINHTTP_QUERY_CUSTOM, pszName,
				Value.data(), &Size, WINHTTP_NO_HEADER_INDEX))
			return String();

		Value.resize(::wcslen(Value.c_str()));

		return Value;
	}

	static bool ReadAllData(HINTERNET hRequest, std::vector<BYTE> *pBody)
	{
		pBody->clear();

		for (;;) {
			DWORD Available = 0;

			if (!::WinHttpQueryDataAvailable(hRequest, &Available))
				return false;
			if (Available == 0)
				break;

			const size_t Offset = pBody->size();

			if (Offset + Available > MAX_RESPONSE_SIZE)
				return false;

			try {
				pBody->resize(Offset + Available);
			} catch (const std::bad_alloc &) {
				return false;
			}

			DWORD Read = 0;
			if (!::WinHttpReadData(hRequest, pBody->data() + Offset, Available, &Read)) {
				pBody->resize(Offset);
				return false;
			}

			pBody->resize(Offset + Read);

			if (Read == 0)
				break;
		}

		return true;
	}

	static unsigned long long StringToUInt64Safe(const String &Str)
	{
		unsigned long long Value = 0;

		for (const WCHAR c : Str) {
			if ((c < L'0') || (c > L'9'))
				break;
			Value = Value * 10 + (c - L'0');
		}

		return Value;
	}

	UrlParts m_Url;
	HINTERNET m_hSession = nullptr;
	HINTERNET m_hConnect = nullptr;
	HINTERNET m_hStream = nullptr;
	bool m_fStreamCanceled = false;
	mutable std::mutex m_StreamLock;
};


/** サーバから見えるサービスの状態 */
struct RemoteService
{
	WORD NetworkID = 0;
	WORD TransportStreamID = 0;
	WORD ServiceID = 0;
	unsigned long long Version = 0;
	unsigned int EventCount = 0;
	String ETag;
};


/** 空白区切りの行を分解する */
std::vector<String> SplitFields(const String &Line, size_t MaxFields)
{
	std::vector<String> Fields;
	size_t Pos = 0;

	while (Pos < Line.length()) {
		while ((Pos < Line.length()) && (Line[Pos] == L' '))
			Pos++;
		if (Pos >= Line.length())
			break;

		if (Fields.size() + 1 == MaxFields) {
			// 最後の項目は空白を含みうる
			Fields.push_back(Line.substr(Pos));
			break;
		}

		const size_t End = Line.find(L' ', Pos);
		if (End == String::npos) {
			Fields.push_back(Line.substr(Pos));
			break;
		}

		Fields.push_back(Line.substr(Pos, End - Pos));
		Pos = End;
	}

	return Fields;
}


bool ParseUInt64(const String &Str, unsigned long long *pValue)
{
	if (Str.empty())
		return false;

	unsigned long long Value = 0;

	for (const WCHAR c : Str) {
		if ((c < L'0') || (c > L'9'))
			return false;
		Value = Value * 10 + (c - L'0');
	}

	*pValue = Value;

	return true;
}


}	// namespace




class CEpgSyncClient::CImpl
	: public LibISDB::EPGDatabase::EventListener
{
public:
	CImpl(LibISDB::EPGDatabase *pEPGDatabase, const SyncSettings &Settings,
		  std::string ServiceMetadata, std::vector<LogoImage> Logos,
		  CEventHandler *pEventHandler)
		: m_pEPGDatabase(pEPGDatabase)
		, m_Settings(Settings)
		, m_ServiceMetadata(std::move(ServiceMetadata))
		, m_Logos(std::move(Logos))
		, m_pEventHandler(pEventHandler)
	{
	}

	~CImpl() { Stop(); }

	bool Start()
	{
		UrlParts Url;

		if (!ParseUrl(m_Settings.Server, &Url)) {
			GetAppClass().AddLog(
				CLogItem::LogType::Error,
				TEXT("EPG 共有サーバの URL が不正です : {}"), m_Settings.Server);
			return false;
		}

		BuildHeaders();

		m_hStopEvent = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
		m_hSendEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
		m_hSendCompleteEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if ((m_hStopEvent == nullptr) || (m_hSendEvent == nullptr)
				|| (m_hSendCompleteEvent == nullptr)) {
			Stop();
			return false;
		}

		if (!m_SendClient.Open(Url) || !m_ReceiveClient.Open(Url)) {
			GetAppClass().AddLog(
				CLogItem::LogType::Error,
				TEXT("EPG 共有サーバへの接続を準備できません : {}"), m_Settings.Server);
			Stop();
			return false;
		}

		m_pEPGDatabase->AddEventListener(this);

		m_hSendThread = reinterpret_cast<HANDLE>(
			::_beginthreadex(nullptr, 0, SendThread, this, 0, nullptr));
		m_hReceiveThread = reinterpret_cast<HANDLE>(
			::_beginthreadex(nullptr, 0, ReceiveThread, this, 0, nullptr));

		if ((m_hSendThread == nullptr) || (m_hReceiveThread == nullptr)) {
			Stop();
			return false;
		}

		GetAppClass().AddLog(
			TEXT("EPG 共有サーバ \"{}\" との同期を開始します。"), m_Settings.Server);

		return true;
	}

	void Stop()
	{
		if (m_hStopEvent != nullptr)
			::SetEvent(m_hStopEvent);

		// 読み込み中のスレッドを起こす
		m_ReceiveClient.CancelStream();

		if (m_pEPGDatabase != nullptr)
			m_pEPGDatabase->RemoveEventListener(this);

		WaitThread(&m_hSendThread);
		WaitThread(&m_hReceiveThread);

		m_SendClient.Close();
		m_ReceiveClient.Close();

		CloseHandleSafe(&m_hStopEvent);
		CloseHandleSafe(&m_hSendEvent);
		CloseHandleSafe(&m_hSendCompleteEvent);
	}

	bool IsRunning() const { return m_hSendThread != nullptr; }

	void SetEventHandler(CEventHandler *pEventHandler) { m_pEventHandler = pEventHandler; }

	void RequestSend()
	{
		if (m_hSendEvent != nullptr) {
			m_SendRequestSerial.fetch_add(1, std::memory_order_release);
			::SetEvent(m_hSendEvent);
		}
	}

	bool Flush(DWORD Timeout)
	{
		if ((m_hSendEvent == nullptr) || (m_hSendCompleteEvent == nullptr))
			return false;

		const unsigned long long Target =
			m_SendRequestSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
		::SetEvent(m_hSendEvent);

		const DWORD Start = ::GetTickCount();

		for (;;) {
			if (m_SendCompleteSerial.load(std::memory_order_acquire) >= Target)
				return !HasUpdatedServices();

			const DWORD Elapsed = ::GetTickCount() - Start;
			if (Elapsed >= Timeout)
				return false;

			if (::WaitForSingleObject(m_hSendCompleteEvent, Timeout - Elapsed)
					!= WAIT_OBJECT_0)
				return false;
		}
	}

// LibISDB::EPGDatabase::EventListener
	void OnServiceCompleted(
		LibISDB::EPGDatabase *pEPGDatabase,
		uint16_t NetworkID, uint16_t TransportStreamID, uint16_t ServiceID,
		bool IsExtended) override
	{
		RequestSend();
	}

private:
	static unsigned int __stdcall SendThread(void *pParameter)
	{
		static_cast<CImpl *>(pParameter)->SendThreadMain();
		return 0;
	}

	static unsigned int __stdcall ReceiveThread(void *pParameter)
	{
		static_cast<CImpl *>(pParameter)->ReceiveThreadMain();
		return 0;
	}

	// -- 送信側 ------------------------------------------------------------

	void SendThreadMain()
	{
		::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

		// ローカルの EpgData を読み終えるのを待つ。
		// 待たずに比較すると、既に持っている番組情報をサーバから取り直してしまう。
		GetAppClass().EpgOptions.WaitEpgFileLoad(60 * 1000);

		// 番組表で局名とチャンネル順を表示できるよう、起動時の一覧を送る。
		if (!IsStopped())
			PushServiceMetadata();

		// 局ロゴは滅多に変わらないので、サーバに無いものだけを送る。
		if (!IsStopped())
			PushLogos();

		// まずサーバの持ち物を取り込む
		if (!IsStopped())
			PullAll();

		while (!IsStopped()) {
			const HANDLE Handles[] = {m_hStopEvent, m_hSendEvent};
			const DWORD Result = ::WaitForMultipleObjects(
				static_cast<DWORD>(std::size(Handles)), Handles, FALSE, m_Settings.ScanInterval);

			if (Result == WAIT_OBJECT_0)
				break;

			const unsigned long long Target =
				m_SendRequestSerial.load(std::memory_order_acquire);
			PushUpdatedServices();
			m_SendCompleteSerial.store(Target, std::memory_order_release);
			::SetEvent(m_hSendCompleteEvent);
		}
	}

	bool HasUpdatedServices() const
	{
		LibISDB::EPGDatabase::ServiceList ServiceList;

		if (!m_pEPGDatabase->GetServiceList(&ServiceList))
			return true;

		for (const LibISDB::EPGDatabase::ServiceInfo &Service : ServiceList) {
			if (m_pEPGDatabase->IsServiceUpdated(
					Service.NetworkID, Service.TransportStreamID, Service.ServiceID))
				return true;
		}

		return false;
	}

	void PushServiceMetadata()
	{
		if (m_ServiceMetadata.empty())
			return;

		String Headers = m_CommonHeaders;
		Headers += TEXT("Content-Type: application/json; charset=utf-8\r\n");

		CHttpClient::Response Response;
		if (!m_SendClient.Request(
				L"PUT", TEXT("/api/service-metadata"), Headers,
				m_ServiceMetadata.data(), m_ServiceMetadata.size(), &Response)) {
			GetAppClass().AddLog(
				CLogItem::LogType::Warning,
				TEXT("EPG 共有サーバへ局情報を送信できませんでした。"));
			return;
		}

		if (Response.StatusCode == 200) {
			GetAppClass().AddLog(TEXT("EPG 共有サーバへ局情報を送信しました。"));
		} else if (Response.StatusCode != 404) {
			GetAppClass().AddLog(
				CLogItem::LogType::Warning,
				TEXT("EPG 共有サーバへの局情報送信が拒否されました (HTTP {})。"),
				Response.StatusCode);
		}
	}

	/** サーバが持っていないロゴを送る */
	void PushLogos()
	{
		if (m_Logos.empty())
			return;

		std::map<unsigned long long, unsigned int> Remote;
		if (!FetchLogoList(&Remote))
			return;

		String Headers = m_CommonHeaders;
		Headers += TEXT("Content-Type: image/png\r\n");

		int Sent = 0;

		for (const LogoImage &Logo : m_Logos) {
			if (IsStopped())
				break;

			const unsigned long long Key =
				(static_cast<unsigned long long>(Logo.NetworkID) << 32)
				| (static_cast<unsigned long long>(Logo.LogoID) << 16)
				| Logo.LogoType;
			const auto itr = Remote.find(Key);
			// バージョンは 4bit で一周するので、一致しないときだけ送る。
			if ((itr != Remote.end()) && (itr->second == Logo.LogoVersion))
				continue;

			String Path;
			StringFormat(
				&Path, TEXT("/api/logo/{}/{}/{}"),
				Logo.NetworkID, Logo.LogoID, Logo.LogoType);

			// StringFormat() は代入なので、組み立ててから足す。
			String VersionHeader;
			StringFormat(
				&VersionHeader, TEXT("X-EPG-Logo-Version: {}\r\n"), Logo.LogoVersion);
			const String LogoHeaders = Headers + VersionHeader;

			CHttpClient::Response Response;
			if (!m_SendClient.Request(
					L"PUT", Path, LogoHeaders,
					Logo.Data.data(), Logo.Data.size(), &Response))
				break;
			if (Response.StatusCode == 404)
				return; // ロゴに対応していないサーバ
			if (Response.StatusCode == 200)
				Sent++;
		}

		if (Sent > 0)
			GetAppClass().AddLog(TEXT("EPG 共有サーバへ {} 個の局ロゴを送信しました。"), Sent);
	}

	/** サーバが持っているロゴの一覧を得る */
	bool FetchLogoList(std::map<unsigned long long, unsigned int> *pList)
	{
		CHttpClient::Response Response;

		if (!m_SendClient.Request(
				L"GET", TEXT("/api/logos?format=text"), m_CommonHeaders,
				nullptr, 0, &Response))
			return false;
		if (Response.StatusCode != 200)
			return false;

		const String Text = FromUTF8(
			reinterpret_cast<const char *>(Response.Body.data()), Response.Body.size());
		size_t Pos = 0;

		while (Pos < Text.length()) {
			size_t End = Text.find(L'\n', Pos);
			if (End == String::npos)
				End = Text.length();

			String Line = Text.substr(Pos, End - Pos);
			Pos = End + 1;

			if (!Line.empty() && (Line.back() == L'\r'))
				Line.pop_back();
			if (Line.empty())
				continue;

			// <nid> <logo_id> <type> <version>
			const std::vector<String> Fields = SplitFields(Line, 4);
			if (Fields.size() < 4)
				continue;

			unsigned long long Values[4];
			bool fOK = true;

			for (int i = 0; i < 4; i++) {
				if (!ParseUInt64(Fields[i], &Values[i])) {
					fOK = false;
					break;
				}
			}
			if (!fOK || (Values[0] > 0xFFFF) || (Values[1] > 0xFFFF) || (Values[2] > 0xFF))
				continue;

			(*pList)[(Values[0] << 32) | (Values[1] << 16) | Values[2]] =
				static_cast<unsigned int>(Values[3]);
		}

		return true;
	}

	/** サーバにあって自分に無い(または自分より新しい)サービスを取り込む */
	void PullAll()
	{
		std::vector<RemoteService> RemoteList;

		if (!FetchServiceList(&m_SendClient, &RemoteList))
			return;

		int Merged = 0;

		for (const RemoteService &Remote : RemoteList) {
			if (IsStopped())
				break;

			if (!ShouldPull(
					Remote.NetworkID, Remote.TransportStreamID, Remote.ServiceID,
					Remote.Version, Remote.EventCount))
				continue;

			if (PullService(
					&m_SendClient,
					Remote.NetworkID, Remote.TransportStreamID, Remote.ServiceID))
				Merged++;
		}

		GetAppClass().AddLog(
			TEXT("EPG 共有サーバから {} / {} サービスを取り込みました。"),
			Merged, RemoteList.size());
	}

	/** 自分が受信して更新されたサービスをサーバへ送る */
	void PushUpdatedServices()
	{
		LibISDB::EPGDatabase::ServiceList ServiceList;

		if (!m_pEPGDatabase->GetServiceList(&ServiceList))
			return;

		for (const LibISDB::EPGDatabase::ServiceInfo &Service : ServiceList) {
			if (IsStopped())
				break;

			// EIT から受信したサービスだけが対象になる。
			// 他機から取り込んだものは MergeFlag::SetServiceUpdated を
			// 指定していないため、ここには現れない(送り返しの防止)。
			if (!m_pEPGDatabase->IsServiceUpdated(
					Service.NetworkID, Service.TransportStreamID, Service.ServiceID))
				continue;

			PushService(Service);
		}
	}

	void PushService(const LibISDB::EPGDatabase::ServiceInfo &Service)
	{
		for (int Retry = 0; Retry < PUT_RETRY_COUNT; Retry++) {
			if (IsStopped())
				return;

			// read-modify-write
			// サーバ側の内容を取り込んでから、統合結果を書き戻す。
			// 既存の CEpgDataStore::Save() が UpdateCount を見て
			// LoadMerged() してから書くのと同じ考え方。
			String ETag;
			if (!PullService(
					&m_SendClient,
					Service.NetworkID, Service.TransportStreamID, Service.ServiceID, &ETag))
				ETag.clear();

			const unsigned long long PushedVersion =
				GetLocalVersion(Service.NetworkID, Service.TransportStreamID, Service.ServiceID);

			LibISDB::MemoryStream Stream;

			if (!LibISDB::EPGDataSerializer::SerializeService(
					*m_pEPGDatabase,
					Service.NetworkID, Service.TransportStreamID, Service.ServiceID,
					Stream)) {
				return;
			}

			String Headers = m_CommonHeaders;
			if (!ETag.empty()) {
				String MatchHeader;
				StringFormat(&MatchHeader, TEXT("If-Match: {}\r\n"), ETag);
				Headers += MatchHeader;
			}

			CHttpClient::Response Response;

			if (!m_SendClient.Request(
					L"PUT", MakeServicePath(Service), Headers,
					Stream.GetData(), Stream.GetDataSize(), &Response)) {
				LogOnce(TEXT("EPG 共有サーバへの送信に失敗しました。"));
				return;
			}

			if (Response.StatusCode == 412) {
				// 他の機が先に更新した。取り直してやり直す。
				continue;
			}

			if ((Response.StatusCode != 200) && (Response.StatusCode != 409)) {
				LogOnce(
					TEXT("EPG 共有サーバへの送信が拒否されました (HTTP {})。"),
					Response.StatusCode);
				return;
			}

			// 送信中に新たな受信があった場合は更新済みのままにしておく
			const unsigned long long CurrentVersion =
				GetLocalVersion(Service.NetworkID, Service.TransportStreamID, Service.ServiceID);

			if (CurrentVersion == PushedVersion) {
				m_pEPGDatabase->ResetServiceUpdated(
					Service.NetworkID, Service.TransportStreamID, Service.ServiceID);
			}

			m_fLogged = false;

			return;
		}
	}

	// -- 受信側 ------------------------------------------------------------

	void ReceiveThreadMain()
	{
		::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

		DWORD Interval = RECONNECT_INTERVAL_MIN;

		while (!IsStopped()) {
			if (m_ReceiveClient.BeginStream(TEXT("/api/events?format=text"), m_CommonHeaders)) {
				Interval = RECONNECT_INTERVAL_MIN;
				ReadEventStream();
				m_ReceiveClient.EndStream();
			}

			if (IsStopped())
				break;

			// 切断されたら間隔を空けて再接続する
			if (::WaitForSingleObject(m_hStopEvent, Interval) == WAIT_OBJECT_0)
				break;

			Interval = std::min(Interval * 2, RECONNECT_INTERVAL_MAX);
		}
	}

	void ReadEventStream()
	{
		std::string Buffer;
		char ReadBuffer[4096];

		for (;;) {
			DWORD Read = 0;

			if (!m_ReceiveClient.ReadStream(ReadBuffer, sizeof(ReadBuffer), &Read))
				return;

			Buffer.append(ReadBuffer, Read);

			for (;;) {
				const size_t Pos = Buffer.find('\n');
				if (Pos == std::string::npos)
					break;

				std::string Line = Buffer.substr(0, Pos);
				Buffer.erase(0, Pos + 1);

				if (!Line.empty() && (Line.back() == '\r'))
					Line.pop_back();

				if (Line.compare(0, 6, "data: ") == 0)
					OnEventLine(FromUTF8(Line.data() + 6, Line.length() - 6));
			}

			if (Buffer.length() > 64 * 1024) {
				// 行として解釈できないものが溜まり続けている
				return;
			}

			if (IsStopped())
				return;
		}
	}

	void OnEventLine(const String &Line)
	{
		// updated <nid> <tsid> <sid> <version> <event_count> <etag> <source>
		const std::vector<String> Fields = SplitFields(Line, 8);

		if ((Fields.size() < 5) || (Fields[0] != TEXT("updated")))
			return;

		unsigned long long NetworkID, TransportStreamID, ServiceID, Version;

		if (!ParseUInt64(Fields[1], &NetworkID)
				|| !ParseUInt64(Fields[2], &TransportStreamID)
				|| !ParseUInt64(Fields[3], &ServiceID)
				|| !ParseUInt64(Fields[4], &Version))
			return;
		if ((NetworkID > 0xFFFF) || (TransportStreamID > 0xFFFF) || (ServiceID > 0xFFFF))
			return;

		unsigned long long EventCount = 0;
		if ((Fields.size() < 6) || !ParseUInt64(Fields[5], &EventCount))
			EventCount = 0;

		// 自分が送ったものは無視する
		if ((Fields.size() >= 8) && !m_Settings.Name.empty()
				&& (Fields[7] == m_Settings.Name))
			return;

		if (!ShouldPull(
				static_cast<WORD>(NetworkID),
				static_cast<WORD>(TransportStreamID),
				static_cast<WORD>(ServiceID),
				Version, static_cast<unsigned int>(EventCount)))
			return;

		if (PullService(
				&m_ReceiveClient,
				static_cast<WORD>(NetworkID),
				static_cast<WORD>(TransportStreamID),
				static_cast<WORD>(ServiceID))) {
			GetAppClass().AddLog(
				TEXT("EPG 共有サーバから {:04X}/{:04X}/{:04X} の番組情報を取り込みました ({} 番組, from {})。"),
				NetworkID, TransportStreamID, ServiceID,
				EventCount, Fields.size() >= 8 ? Fields[7] : String());
		}
	}

	// -- 共通 --------------------------------------------------------------

	bool FetchServiceList(CHttpClient *pClient, std::vector<RemoteService> *pList)
	{
		CHttpClient::Response Response;

		if (!pClient->Request(
				L"GET", TEXT("/api/services?format=text"), m_CommonHeaders,
				nullptr, 0, &Response)) {
			LogOnce(TEXT("EPG 共有サーバに接続できません : {}"), m_Settings.Server);
			return false;
		}

		if (Response.StatusCode != 200) {
			LogOnce(
				TEXT("EPG 共有サーバがサービス一覧を返しませんでした (HTTP {})。"),
				Response.StatusCode);
			return false;
		}

		m_fLogged = false;

		const String Text = FromUTF8(
			reinterpret_cast<const char *>(Response.Body.data()), Response.Body.size());

		size_t Pos = 0;

		while (Pos < Text.length()) {
			size_t End = Text.find(L'\n', Pos);
			if (End == String::npos)
				End = Text.length();

			String Line = Text.substr(Pos, End - Pos);
			Pos = End + 1;

			if (!Line.empty() && (Line.back() == L'\r'))
				Line.pop_back();
			if (Line.empty())
				continue;

			// <nid> <tsid> <sid> <version> <event_count> <etag>
			const std::vector<String> Fields = SplitFields(Line, 6);
			if (Fields.size() < 6)
				continue;

			unsigned long long Values[5];
			bool fOK = true;

			for (int i = 0; i < 5; i++) {
				if (!ParseUInt64(Fields[i], &Values[i])) {
					fOK = false;
					break;
				}
			}
			if (!fOK || (Values[0] > 0xFFFF) || (Values[1] > 0xFFFF) || (Values[2] > 0xFFFF))
				continue;

			RemoteService Service;

			Service.NetworkID = static_cast<WORD>(Values[0]);
			Service.TransportStreamID = static_cast<WORD>(Values[1]);
			Service.ServiceID = static_cast<WORD>(Values[2]);
			Service.Version = Values[3];
			Service.EventCount = static_cast<unsigned int>(Values[4]);
			Service.ETag = Fields[5];

			pList->push_back(Service);
		}

		return true;
	}

	bool PullService(
		CHttpClient *pClient,
		WORD NetworkID, WORD TransportStreamID, WORD ServiceID, String *pETag = nullptr)
	{
		CHttpClient::Response Response;

		if (!pClient->Request(
				L"GET", MakeServicePath(NetworkID, TransportStreamID, ServiceID),
				m_CommonHeaders, nullptr, 0, &Response))
			return false;

		if (Response.StatusCode != 200)
			return false;

		if (pETag != nullptr)
			*pETag = Response.ETag;

		LibISDB::MemoryStream Stream;

		if (!Stream.SetData(Response.Body.data(), Response.Body.size()))
			return false;

		LibISDB::EPGDatabase TempDatabase;

		if (!LibISDB::EPGDataSerializer::DeserializeService(Stream, TempDatabase)) {
			GetAppClass().AddLog(
				CLogItem::LogType::Warning,
				TEXT("EPG 共有サーバから受け取ったデータを解釈できません ({:04X}/{:04X}/{:04X})。"),
				NetworkID, TransportStreamID, ServiceID);
			return false;
		}

		// MergeFlag::SetServiceUpdated は指定しない。
		// 指定すると受け取ったデータを送り返してしまう。
		if (!m_pEPGDatabase->MergeService(
				&TempDatabase, NetworkID, TransportStreamID, ServiceID,
				LibISDB::EPGDatabase::MergeFlag::Database
					| LibISDB::EPGDatabase::MergeFlag::MergeBasicExtended))
			return false;

		if (m_pEventHandler != nullptr)
			m_pEventHandler->OnServiceMerged(NetworkID, TransportStreamID, ServiceID);

		return true;
	}

	/** ローカルのサービスのバージョン(UpdatedTime の最大値)と番組数を求める

	番組数は EPGDataSerializer が書き出す条件に合わせて数える。
	*/
	void GetLocalState(
		WORD NetworkID, WORD TransportStreamID, WORD ServiceID,
		unsigned long long *pVersion, unsigned int *pEventCount) const
	{
		unsigned long long Version = 0;
		unsigned int EventCount = 0;

		m_pEPGDatabase->EnumEventsUnsorted(
			NetworkID, TransportStreamID, ServiceID,
			[&Version, &EventCount](const LibISDB::EventInfo &Event) -> bool {
				if (Event.EventName.empty() && !Event.IsCommonEvent)
					return true;
				if (Event.UpdatedTime > Version)
					Version = Event.UpdatedTime;
				EventCount++;
				return true;
			});

		if (pVersion != nullptr)
			*pVersion = Version;
		if (pEventCount != nullptr)
			*pEventCount = EventCount;
	}

	unsigned long long GetLocalVersion(WORD NetworkID, WORD TransportStreamID, WORD ServiceID) const
	{
		unsigned long long Version = 0;

		GetLocalState(NetworkID, TransportStreamID, ServiceID, &Version, nullptr);

		return Version;
	}

	/** サーバ側の情報を取り込むべきか

	バージョンだけで判断すると、番組数が少なくても受信直後で UpdatedTime が
	新しいサービスは、他機が持っている豊富な情報を取り込めなくなる。
	相手のほうが番組を多く持っている場合も取り込む。
	*/
	bool ShouldPull(
		WORD NetworkID, WORD TransportStreamID, WORD ServiceID,
		unsigned long long RemoteVersion, unsigned int RemoteEventCount) const
	{
		unsigned long long LocalVersion;
		unsigned int LocalEventCount;

		GetLocalState(
			NetworkID, TransportStreamID, ServiceID, &LocalVersion, &LocalEventCount);

		return (RemoteVersion > LocalVersion) || (RemoteEventCount > LocalEventCount);
	}

	void BuildHeaders()
	{
		m_CommonHeaders.clear();

		String Header;

		if (!m_Settings.Token.empty()) {
			StringFormat(&Header, TEXT("X-EPG-Token: {}\r\n"), m_Settings.Token);
			m_CommonHeaders += Header;
		}
		if (!m_Settings.Name.empty()) {
			StringFormat(&Header, TEXT("X-EPG-Source: {}\r\n"), m_Settings.Name);
			m_CommonHeaders += Header;
		}
	}

	static String MakeServicePath(WORD NetworkID, WORD TransportStreamID, WORD ServiceID)
	{
		String Path;

		StringFormat(
			&Path, TEXT("/api/service/{}/{}/{}"), NetworkID, TransportStreamID, ServiceID);

		return Path;
	}

	static String MakeServicePath(const LibISDB::EPGDatabase::ServiceInfo &Service)
	{
		return MakeServicePath(Service.NetworkID, Service.TransportStreamID, Service.ServiceID);
	}

	bool IsStopped() const
	{
		return (m_hStopEvent == nullptr)
			|| (::WaitForSingleObject(m_hStopEvent, 0) == WAIT_OBJECT_0);
	}

	/** 同じ失敗を繰り返しログに出さないようにする */
	template<typename... TArgs> void LogOnce(StringView Format, TArgs&&... Args)
	{
		if (m_fLogged)
			return;
		m_fLogged = true;
		GetAppClass().AddLog(CLogItem::LogType::Warning, Format, Args...);
	}

	static void WaitThread(HANDLE *phThread)
	{
		if (*phThread != nullptr) {
			if (::WaitForSingleObject(*phThread, 15000) == WAIT_TIMEOUT) {
				GetAppClass().AddLog(
					CLogItem::LogType::Warning,
					TEXT("EPG 共有スレッドを強制終了します。"));
				::TerminateThread(*phThread, static_cast<DWORD>(-1));
			}
			::CloseHandle(*phThread);
			*phThread = nullptr;
		}
	}

	static void CloseHandleSafe(HANDLE *phHandle)
	{
		if (*phHandle != nullptr) {
			::CloseHandle(*phHandle);
			*phHandle = nullptr;
		}
	}

	LibISDB::EPGDatabase *m_pEPGDatabase;
	SyncSettings m_Settings;
	std::string m_ServiceMetadata;
	std::vector<LogoImage> m_Logos;
	CEventHandler *m_pEventHandler = nullptr;
	String m_CommonHeaders;
	CHttpClient m_SendClient;
	CHttpClient m_ReceiveClient;
	HANDLE m_hSendThread = nullptr;
	HANDLE m_hReceiveThread = nullptr;
	HANDLE m_hStopEvent = nullptr;
	HANDLE m_hSendEvent = nullptr;
	HANDLE m_hSendCompleteEvent = nullptr;
	std::atomic<unsigned long long> m_SendRequestSerial{0};
	std::atomic<unsigned long long> m_SendCompleteSerial{0};
	bool m_fLogged = false;
};




CEpgSyncClient::CEpgSyncClient() = default;


CEpgSyncClient::~CEpgSyncClient()
{
	Close();
}


bool CEpgSyncClient::Open(LibISDB::EPGDatabase *pEPGDatabase, const SyncSettings &Settings)
{
	Close();

	if (pEPGDatabase == nullptr)
		return false;
	if (!Settings.IsValid())
		return false;

	m_Settings = Settings;

	std::vector<LogoImage> Logos;
	std::string ServiceMetadata = BuildServiceMetadata(&Logos);

	auto Impl = std::make_unique<CImpl>(
		pEPGDatabase, m_Settings, std::move(ServiceMetadata), std::move(Logos),
		m_pEventHandler);

	if (!Impl->Start())
		return false;

	m_Impl = std::move(Impl);

	return true;
}


void CEpgSyncClient::Close()
{
	if (m_Impl) {
		m_Impl->Stop();
		m_Impl.reset();
	}
}


bool CEpgSyncClient::IsOpen() const
{
	return static_cast<bool>(m_Impl);
}


void CEpgSyncClient::SetEventHandler(CEventHandler *pEventHandler)
{
	m_pEventHandler = pEventHandler;

	if (m_Impl)
		m_Impl->SetEventHandler(pEventHandler);
}


void CEpgSyncClient::RequestSend()
{
	if (m_Impl)
		m_Impl->RequestSend();
}


bool CEpgSyncClient::Flush(DWORD Timeout)
{
	if (!m_Settings.IsValid())
		return true;

	return m_Impl && m_Impl->Flush(Timeout);
}


}	// namespace TVTest
