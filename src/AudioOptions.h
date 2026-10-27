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


#ifndef TVTEST_AUDIO_OPTIONS_H
#define TVTEST_AUDIO_OPTIONS_H


#include "Options.h"
#include "LibISDB/LibISDB/Windows/Viewer/ViewerFilter.hpp"
#include "LibISDB/LibISDB/Windows/Viewer/DirectShow/DirectShowUtilities.hpp"
#include <vector>


namespace TVTest
{

	class CAudioOptions
		: public COptions
	{
	public:
		struct AudioLanguageInfo
		{
			DWORD Language;
			bool fSub;

			bool operator==(const AudioLanguageInfo &Op) const noexcept = default;
		};

		typedef std::vector<AudioLanguageInfo> AudioLanguageList;

		using AACDecoderType = LibISDB::ViewerFilter::AACDecoderType;

		~CAudioOptions();

	// CSettingsBase
		bool ReadSettings(CSettings &Settings) override;
		bool WriteSettings(CSettings &Settings) override;

	// CBasicDialog
		bool Create(HWND hwndOwner) override;

	// CAudioOptions
		bool ApplyMediaViewerOptions();
		/*
			音声の出力先として、DirectShow の音声デバイスの代わりに Windows
			空間オーディオを選べるようにしている。デバイス一覧に並べる擬似的な
			項目で、このモニカ名が選ばれている場合は音声デコーダを Null
			レンダラへ繋ぎ、CSpatialAudioRenderer が再生を受け持つ。
		*/
		static constexpr LPCTSTR SPATIAL_AUDIO_MONIKER_NAME = TEXT("@TVTest:WindowsSpatialAudio");
		static constexpr LPCTSTR SPATIAL_AUDIO_DEVICE_NAME = TEXT("Windows 空間オーディオ");
		bool IsSpatialAudioSelected() const {
			return StringUtility::IsEqualNoCase(m_AudioDevice.MonikerName, SPATIAL_AUDIO_MONIKER_NAME);
		}
		/*
			22.2ch 音声を通常の音声デバイスへ出す場合のダウンミックス先。
			2 / 6 (5.1ch) / 8 (7.1ch) のいずれか。Windows 空間オーディオへ
			出す場合はダウンミックスせず 24ch のまま再生するため使われない。
		*/
		int GetLatmDownmixChannels() const { return m_LatmDownmixChannels; }
		/** 使用するダウンミックスのプリセット名。空ならチャンネル数から決まる既定名 */
		const String & GetLatmDownmixPresetName() const { return m_LatmDownmixPresetName; }
		/** ダウンミックスのプリセットを定義する ini ファイルの名前 */
		static constexpr LPCTSTR LATM_DOWNMIX_PRESET_FILE_NAME = TEXT("LatmDownmixPresets.ini");
		const String & GetAudioDeviceName() const { return m_AudioDevice.FriendlyName; }
		const String & GetAudioDeviceMoniker() const { return m_AudioDevice.MonikerName; }
		LPCTSTR GetAudioFilterName() const { return StringUtility::GetCStrOrNull(m_AudioFilterName); }
		AACDecoderType GetAACDecoder() const { return m_AACDecoder; }
		const LibISDB::DirectShow::AudioDecoderFilter::SPDIFOptions &GetSpdifOptions() const { return m_SPDIFOptions; }
		bool SetSpdifOptions(const LibISDB::DirectShow::AudioDecoderFilter::SPDIFOptions &Options);
		bool GetDownMixSurround() const { return m_fDownMixSurround; }
		bool GetEnableLanguagePriority() const { return m_fEnableLanguagePriority; }
		const AudioLanguageList &GetLanguagePriority() const { return m_LanguagePriority; }
		bool GetResetAudioDelayOnChannelChange() const { return m_fResetAudioDelayOnChannelChange; }

	private:
		class CSurroundOptionsDialog
			: public CBasicDialog
		{
		public:
			CSurroundOptionsDialog(CAudioOptions *pOptions);
			bool Show(HWND hwndOwner) override;

		private:
			CAudioOptions *m_pOptions;

			INT_PTR DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) override;
			void SetDownMixMatrix(const LibISDB::DirectShow::AudioDecoderFilter::DownMixMatrix &Matrix);
		};

		static const LibISDB::DirectShow::AudioDecoderFilter::SurroundMixingMatrix m_DefaultSurroundMixingMatrix;
		static const LibISDB::DirectShow::AudioDecoderFilter::DownMixMatrix m_DefaultDownMixMatrix;
		static const DWORD m_AudioLanguageList[];
		static constexpr DWORD LANGUAGE_FLAG_SUB = 0x01000000;

		using FilterInfo = LibISDB::DirectShow::FilterInfo;

		FilterInfo m_AudioDevice;
		String m_AudioFilterName;
		AACDecoderType m_AACDecoder = AACDecoderType::Default;

		LibISDB::DirectShow::AudioDecoderFilter::SPDIFOptions m_SPDIFOptions{
			LibISDB::DirectShow::AudioDecoderFilter::SPDIFMode::Disabled,
			LibISDB::DirectShow::AudioDecoderFilter::SPDIFOptions::Channel_Surround};
		bool m_fDownMixSurround = true;
		int m_LatmDownmixChannels = 2;
		String m_LatmDownmixPresetName;
		bool m_fUseCustomSurroundMixingMatrix = false;
		LibISDB::DirectShow::AudioDecoderFilter::SurroundMixingMatrix m_SurroundMixingMatrix{m_DefaultSurroundMixingMatrix};
		bool m_fUseCustomDownMixMatrix = false;
		LibISDB::DirectShow::AudioDecoderFilter::DownMixMatrix m_DownMixMatrix{m_DefaultDownMixMatrix};
		bool m_fEnableLanguagePriority = false;
		AudioLanguageList m_LanguagePriority;
		bool m_fResetAudioDelayOnChannelChange = true;

		std::vector<FilterInfo> m_AudioDeviceList;

	// CBasicDialog
		INT_PTR DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) override;

		void GetLanguageText(DWORD Language, bool fSub, LPTSTR pszText, int MaxText) const;
		void UpdateLanguagePriorityControls();
	};

} // namespace TVTest


#endif
