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
#include <bit>
#include "AppMain.h"
#include "EpgCapture.h"
#include "Common/DebugDef.h"


namespace TVTest
{


namespace
{

// 数値か * を1つ読む(* は -1 として返す)
bool ParseFilterValue(LPCTSTR *ppText, int *pValue)
{
	LPCTSTR p = *ppText;

	if (*p == _T('*')) {
		*pValue = -1;
		*ppText = p + 1;
		return true;
	}
	if (*p < _T('0') || *p > _T('9'))
		return false;

	int Value = 0;
	do {
		Value = Value * 10 + (*p - _T('0'));
		if (Value > 0xFFFF)
			return false;
		p++;
	} while (*p >= _T('0') && *p <= _T('9'));

	*pValue = Value;
	*ppText = p;

	return true;
}

} // namespace


bool CEpgCaptureManager::ChannelFilter::Match(const CChannelInfo &ChannelInfo) const
{
	if (RangeList.empty())
		return true;

	for (const Range &e : RangeList) {
		if (e.Space >= 0 && e.Space != ChannelInfo.GetSpace())
			continue;
		if (e.First < 0)
			return true;
		const int Index = ChannelInfo.GetChannelIndex();
		if (Index >= e.First && Index <= e.Last)
			return true;
	}

	return false;
}


// "空間:チャンネル-チャンネル" をカンマで並べた指定を解析する
// (e.g. "0:0-11,1:*,2")
bool CEpgCaptureManager::ParseChannelFilter(LPCTSTR pszFilter, ChannelFilter *pFilter)
{
	if (pFilter == nullptr)
		return false;

	pFilter->RangeList.clear();

	if (IsStringEmpty(pszFilter))
		return false;

	LPCTSTR p = pszFilter;

	while (*p != _T('\0')) {
		while (*p == _T(' ') || *p == _T(','))
			p++;
		if (*p == _T('\0'))
			break;

		ChannelFilter::Range Range;
		Range.Space = -1;
		Range.First = -1;
		Range.Last = -1;

		if (!ParseFilterValue(&p, &Range.Space))
			return false;
		if (*p == _T(':')) {
			p++;
			if (!ParseFilterValue(&p, &Range.First))
				return false;
			Range.Last = Range.First;
			if (*p == _T('-')) {
				p++;
				if (!ParseFilterValue(&p, &Range.Last))
					return false;
				if (Range.First < 0 || Range.Last < Range.First)
					return false;
			}
		}
		if (*p != _T('\0') && *p != _T(',') && *p != _T(' '))
			return false;

		pFilter->RangeList.push_back(Range);
	}

	return !pFilter->RangeList.empty();
}


bool CEpgCaptureManager::BeginCapture(
	LPCTSTR pszTuner, const CChannelList *pChannelList, BeginFlag Flags)
{
	if (m_fCapturing)
		return false;

	CAppMain &App = GetAppClass();
	const bool fNoUI = !!(Flags & BeginFlag::NoUI);

	if (App.CmdLineOptions.m_fNoEpg) {
		if (!fNoUI) {
			App.UICore.GetSkin()->ShowMessage(
				TEXT("コマンドラインオプションでEPG情報を取得しないように指定されているため、\n番組表の取得ができません。"),
				TEXT("お知らせ"), MB_OK | MB_ICONINFORMATION);
		}
		return false;
	}
	if (App.RecordManager.IsRecording()) {
		if (!fNoUI) {
			App.UICore.GetSkin()->ShowMessage(
				TEXT("録画中は番組表の取得を行えません。"),
				TEXT("お知らせ"), MB_OK | MB_ICONINFORMATION);
		}
		return false;
	}
	if (!IsStringEmpty(pszTuner)) {
		CDriverManager::TunerSpec Spec;
		if (App.DriverManager.GetTunerSpec(pszTuner, &Spec)
				&& !!(Spec.Flags &
					(CDriverManager::TunerSpec::Flag::Network |
					 CDriverManager::TunerSpec::Flag::File))) {
			if (!fNoUI) {
				App.UICore.GetSkin()->ShowMessage(
					TEXT("ネットワーク再生及びファイル再生では番組表の取得はできません。"),
					TEXT("お知らせ"), MB_OK | MB_ICONINFORMATION);
			}
			return false;
		}
	}

	const bool fTunerAlreadyOpened = App.CoreEngine.IsTunerOpen();

	if (!IsStringEmpty(pszTuner)) {
		if (!App.Core.OpenTuner(pszTuner))
			return false;
	}

	if (pChannelList == nullptr) {
		pChannelList = App.ChannelManager.GetCurrentChannelList();
		if (pChannelList == nullptr) {
			if (!fTunerAlreadyOpened)
				App.Core.CloseTuner();
			return false;
		}
	}

	m_ChannelList.clear();

	for (int i = 0; i < pChannelList->NumChannels(); i++) {
		const CChannelInfo *pChInfo = pChannelList->GetChannelInfo(i);

		if (pChInfo->IsEnabled() && m_ChannelFilter.Match(*pChInfo)) {
			const CNetworkDefinition::NetworkType Network =
				App.NetworkDefinition.GetNetworkType(pChInfo->GetNetworkID());
			std::vector<ChannelGroup>::iterator itr;

			for (itr = m_ChannelList.begin(); itr != m_ChannelList.end(); ++itr) {
				if (pChInfo->GetSpace() == itr->Space && pChInfo->GetChannelIndex() == itr->Channel)
					break;
				if (pChInfo->GetNetworkID() == itr->ChannelList.GetChannelInfo(0)->GetNetworkID()
						&& ((Network == CNetworkDefinition::NetworkType::BS && !App.EpgOptions.GetUpdateBSExtended())
							|| (Network == CNetworkDefinition::NetworkType::CS && !App.EpgOptions.GetUpdateCSExtended())))
					break;
			}
			if (itr == m_ChannelList.end()) {
				m_ChannelList.emplace_back();
				itr = m_ChannelList.end();
				--itr;
				itr->Space = pChInfo->GetSpace();
				itr->Channel = pChInfo->GetChannelIndex();
				itr->Time = 0;
			}
			itr->ChannelList.AddChannel(*pChInfo);
		}
	}
	if (m_ChannelFilter.PartCount > 1) {
		// 同じトランスポンダのまとまりを崩さないよう、グループ単位で振り分ける
		std::vector<ChannelGroup> Part;

		for (size_t i = 0; i < m_ChannelList.size(); i++) {
			if (static_cast<int>(i % m_ChannelFilter.PartCount) == m_ChannelFilter.PartIndex - 1)
				Part.push_back(std::move(m_ChannelList[i]));
		}
		m_ChannelList = std::move(Part);
	}

	if (m_ChannelList.empty()) {
		App.AddLog(
			CLogItem::LogType::Error,
			TEXT("番組表を取得する対象のチャンネルがありません。"));
		if (!fTunerAlreadyOpened)
			App.Core.CloseTuner();
		return false;
	}

	BeginStatus Status = BeginStatus::None;
	if (fTunerAlreadyOpened)
		Status |= BeginStatus::TunerAlreadyOpened;

	if (App.UICore.GetStandby()) {
		if (!App.Core.OpenTuner())
			return false;
		Status |= BeginStatus::Standby;
	} else {
		if (!App.CoreEngine.IsTunerOpen())
			return false;
		if (!!(Flags & BeginFlag::Standby))
			Status |= BeginStatus::Standby;
	}

	m_fCapturing = true;
	m_fAllChannelsComplete = true;
	m_LastResult = Result::None;
	m_CurChannel = -1;
	m_TotalClock.Start();

	if (m_ChannelFilter.PartCount > 1) {
		App.AddLog(
			TEXT("番組表の取得を開始します。({} チャンネル / {} 分割の {} 番目)"),
			static_cast<int>(m_ChannelList.size()),
			m_ChannelFilter.PartCount, m_ChannelFilter.PartIndex);
	} else if (!m_ChannelFilter.RangeList.empty()) {
		App.AddLog(
			TEXT("番組表の取得を開始します。({} チャンネル)"),
			static_cast<int>(m_ChannelList.size()));
	} else {
		App.AddLog(TEXT("番組表の取得を開始します。"));
	}

	if (m_pEventHandler != nullptr)
		m_pEventHandler->OnBeginCapture(Flags, Status);

	NextChannel();

	return m_fCapturing;
}


void CEpgCaptureManager::EndCapture(EndFlag Flags)
{
	if (!m_fCapturing)
		return;
	if (m_LastResult == Result::None)
		m_LastResult = Result::Canceled;

	CAppMain &App = GetAppClass();

	App.AddLog(TEXT("番組表の取得を終了します。"));

	m_fCapturing = false;
	m_CurChannel = -1;
	m_ChannelList.clear();

	if (m_pEventHandler != nullptr)
		m_pEventHandler->OnEndCapture(Flags);
}


bool CEpgCaptureManager::ProcessCapture()
{
	if (!m_fCapturing)
		return true;

	if (m_Timeout > 0 && m_TotalClock.GetSpan() >= m_Timeout) {
		GetAppClass().AddLog(
			TEXT("番組表の取得が制限時間({}秒)を超えたため中止します。"), m_Timeout / 1000);
		EndCapture();
		return true;
	}

	const CAppMain &App = GetAppClass();
	const LibISDB::EPGDatabase &EPGDatabase = App.EPGDatabase;

	const CChannelList *pChannelList = App.ChannelManager.GetCurrentChannelList();
	const CChannelInfo *pCurChannelInfo = App.ChannelManager.GetCurrentChannelInfo();
	if (pChannelList == nullptr || pCurChannelInfo == nullptr) {
		EndCapture();
		return true;
	}

	bool fComplete = true, fBasic = false, fNoBasic = false;
	ChannelGroup &CurChGroup = m_ChannelList[m_CurChannel];
	for (int i = 0; i < CurChGroup.ChannelList.NumChannels(); i++) {
		const CChannelInfo *pChannelInfo = CurChGroup.ChannelList.GetChannelInfo(i);
		const WORD NetworkID = pChannelInfo->GetNetworkID();
		const WORD TSID = pChannelInfo->GetTransportStreamID();
		const WORD ServiceID = pChannelInfo->GetServiceID();
		const CNetworkDefinition::NetworkType Network =
			App.NetworkDefinition.GetNetworkType(NetworkID);

		if (EPGDatabase.HasSchedule(NetworkID, TSID, ServiceID, false)) {
			fBasic = true;
			if (!EPGDatabase.IsScheduleComplete(NetworkID, TSID, ServiceID, false)) {
				fComplete = false;
				break;
			}
			if ((Network != CNetworkDefinition::NetworkType::BS && Network != CNetworkDefinition::NetworkType::CS)
					|| (Network == CNetworkDefinition::NetworkType::BS && App.EpgOptions.GetUpdateBSExtended())
					|| (Network == CNetworkDefinition::NetworkType::CS && App.EpgOptions.GetUpdateCSExtended())) {
				if (EPGDatabase.HasSchedule(NetworkID, TSID, ServiceID, true)
						&& !EPGDatabase.IsScheduleComplete(NetworkID, TSID, ServiceID, true)) {
					fComplete = false;
					break;
				}
			}
		} else {
			fNoBasic = true;
		}
	}

	if (fComplete && fBasic && fNoBasic
			&& m_AccumulateClock.GetSpan() < 60000)
		fComplete = false;

	if (fComplete) {
		TRACE(TEXT("EPG schedule complete\n"));
	} else {
		const WORD NetworkID = App.CoreEngine.GetNetworkID();
		DWORD Timeout;

		// 真面目に判定する場合BITから周期を取ってくる必要がある
		if (App.NetworkDefinition.IsSatelliteNetworkID(NetworkID))
			Timeout = 360000;
		else
			Timeout = 120000;
		const unsigned int Progress = GetScheduleProgress(CurChGroup);
		if (Progress != m_Progress) {
			m_Progress = Progress;
			m_ProgressClock.Start();
		}

		if (m_AccumulateClock.GetSpan() < Timeout) {
			// 完了と判定できないストリームでは待つだけ無駄なので、
			// 番組表が増えなくなった時点で切り上げる
			if ((m_IdleTimeout == 0) || (Progress == 0)
					|| (m_ProgressClock.GetSpan() < m_IdleTimeout))
				return false;
			GetAppClass().AddLog(
				TEXT("番組表の情報が {} 秒増えないため、次のチャンネルへ移ります。"),
				m_IdleTimeout / 1000);
		}
		TRACE(TEXT("EPG schedule timeout\n"));
		m_fAllChannelsComplete = false;
		LogScheduleStatus(CurChGroup);
	}

	WriteReport(CurChGroup, fComplete, m_AccumulateClock.GetSpan());

	if (m_pEventHandler != nullptr)
		m_pEventHandler->OnChannelEnd(fComplete);

	NextChannel();

	return true;
}


// EIT [schedule] をどこまで受信できたかを表す値
// (増えなくなったことを見るためのもので、値そのものに意味はない)
unsigned int CEpgCaptureManager::GetScheduleProgress(const ChannelGroup &ChGroup) const
{
	const LibISDB::EPGDatabase &EPGDatabase = GetAppClass().EPGDatabase;
	unsigned int Progress = 0;

	for (int i = 0; i < ChGroup.ChannelList.NumChannels(); i++) {
		const CChannelInfo *pChannelInfo = ChGroup.ChannelList.GetChannelInfo(i);

		for (int j = 0; j < 2; j++) {
			LibISDB::EPGDatabase::ScheduleStatus Status;

			if (EPGDatabase.GetScheduleStatus(
					pChannelInfo->GetNetworkID(),
					pChannelInfo->GetTransportStreamID(),
					pChannelInfo->GetServiceID(),
					j != 0, &Status)) {
				for (int k = 0; k < Status.TableCount; k++) {
					Progress +=
						std::popcount(Status.Tables[k].ReceivedSegments) +
						std::popcount(Status.Tables[k].CompleteSegments);
				}
			}
		}
	}

	return Progress;
}


// 完了しなかったチャンネルについて、どのテーブルがどこまで来たかを残す
// (ストリーム側の問題を切り分けるため)
void CEpgCaptureManager::LogScheduleStatus(const ChannelGroup &ChGroup) const
{
	CAppMain &App = GetAppClass();
	const LibISDB::EPGDatabase &EPGDatabase = App.EPGDatabase;

	for (int i = 0; i < ChGroup.ChannelList.NumChannels(); i++) {
		const CChannelInfo *pChannelInfo = ChGroup.ChannelList.GetChannelInfo(i);
		String Text;

		for (int j = 0; j < 2; j++) {
			LibISDB::EPGDatabase::ScheduleStatus Status;

			if (!EPGDatabase.GetScheduleStatus(
					pChannelInfo->GetNetworkID(),
					pChannelInfo->GetTransportStreamID(),
					pChannelInfo->GetServiceID(),
					j != 0, &Status)
					|| (Status.TableCount == 0))
				continue;

			Text += (j == 0) ? TEXT(" basic") : TEXT(" / extended");
			for (int k = 0; k < Status.TableCount; k++) {
				TCHAR szText[64];

				StringFormat(
					szText, TEXT(" 表{} {}/{}"),
					k,
					std::popcount(Status.Tables[k].CompleteSegments),
					std::popcount(Status.Tables[k].ReceivedSegments));
				Text += szText;
			}
		}

		if (Text.empty())
			Text = TEXT(" 受信なし");

		App.AddLog(
			TEXT("番組表が揃いませんでした : {} (SID {}) :{} (揃ったセグメント数/受信したセグメント数 : 全32)"),
			pChannelInfo->GetName(), pChannelInfo->GetServiceID(), Text);
	}
}


// 巡回したチャンネルを1行ずつ追記する
// (取得を分担する外部のプログラムが、どのチャンネルをいつ取得できたかを知るため)
void CEpgCaptureManager::WriteReport(const ChannelGroup &ChGroup, bool fComplete, DWORD Span)
{
	if (m_ReportFileName.empty())
		return;

	const HANDLE hFile = ::CreateFile(
		m_ReportFileName.c_str(), FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) {
		GetAppClass().AddLog(
			CLogItem::LogType::Error,
			TEXT("取得結果を \"{}\" に書き出せません。"), m_ReportFileName);
		m_ReportFileName.clear();
		return;
	}

	SYSTEMTIME st;
	::GetLocalTime(&st);

	TCHAR szText[128];
	const int Length = static_cast<int>(StringFormat(
		szText,
		TEXT("{:04}-{:02}-{:02}T{:02}:{:02}:{:02},{},{},{},{},{}\r\n"),
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
		ChGroup.Space, ChGroup.Channel,
		fComplete ? 1 : 0, (Span + 500) / 1000,
		ChGroup.ChannelList.NumChannels()));

	char Buffer[256];
	const int Size = ::WideCharToMultiByte(
		CP_UTF8, 0, szText, Length, Buffer, sizeof(Buffer), nullptr, nullptr);
	if (Size > 0) {
		DWORD Written;
		::WriteFile(hFile, Buffer, Size, &Written, nullptr);
	}

	::CloseHandle(hFile);
}


void CEpgCaptureManager::SetEventHandler(CEventHandler *pEventHandler)
{
	m_pEventHandler = pEventHandler;
}


DWORD CEpgCaptureManager::GetRemainingTime() const
{
	// TODO: 残り時間をちゃんと算出する
	const CAppMain &App = GetAppClass();
	DWORD Time = 0;

	for (size_t i = m_CurChannel; i < m_ChannelList.size(); i++) {
		const WORD NetworkID = m_ChannelList[i].ChannelList.GetChannelInfo(0)->GetNetworkID();
		if (App.NetworkDefinition.IsSatelliteNetworkID(NetworkID))
			Time += 180000;
		else
			Time += 60000;
	}

	return Time;
}


bool CEpgCaptureManager::NextChannel()
{
	CAppMain &App = GetAppClass();

	for (size_t i = m_CurChannel + 1; i < m_ChannelList.size(); i++) {
		const ChannelGroup &ChGroup = m_ChannelList[i];

		m_fChannelChanging = true;
		const bool fOK = App.Core.SetChannelByIndex(ChGroup.Space, ChGroup.Channel);
		m_fChannelChanging = false;
		if (fOK) {
			m_CurChannel = static_cast<int>(i);
			m_AccumulateClock.Start();
			m_Progress = 0;
			m_ProgressClock.Start();
			if (m_pEventHandler != nullptr)
				m_pEventHandler->OnChannelChanged();
			return true;
		}
		m_fAllChannelsComplete = false;
	}

	m_LastResult = m_fAllChannelsComplete ? Result::Completed : Result::Incomplete;
	EndCapture();

	return false;
}


} // namespace TVTest
