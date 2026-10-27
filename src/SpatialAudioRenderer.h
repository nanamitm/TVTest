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

/**
 @file   SpatialAudioRenderer.h
 @brief  Windows 空間オーディオによる 22.2ch 音声の再生
 @author DBCTRADO
*/


#ifndef TVTEST_SPATIAL_AUDIO_RENDERER_H
#define TVTEST_SPATIAL_AUDIO_RENDERER_H


#include "LibISDB/LibISDB/Windows/Viewer/DirectShow/AudioDecoders/AudioDecoderFilter.hpp"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <wrl/client.h>
#include <spatialaudioclient.h>


namespace TVTest
{

	/**
	 @brief Windows 空間オーディオ (ISpatialAudioClient) による音声出力

	 音声デコーダが Null レンダラへ繋がれている (ViewerFilter の
	 OpenSettings::UseNullAudioRenderer) ときのサンプルコールバックとして
	 使用する。受け取ったインターリーブ PCM をリングバッファへ貯め、
	 専用スレッドで空間オーディオの各オブジェクトへ書き出す。

	 22.2ch (24ch) は、スピーカー位置が決まっている 12 ch を静的
	 オブジェクト (BED) へ、残りを位置付きの動的オブジェクトへ割り当てる。
	 2ch / 5.1ch の音声が来た場合は対応する BED へ流す。
	*/
	class CSpatialAudioRenderer
		: public LibISDB::DirectShow::AudioDecoderFilter::SampleCallback
	{
	public:
		CSpatialAudioRenderer() = default;
		~CSpatialAudioRenderer();

		CSpatialAudioRenderer(const CSpatialAudioRenderer &) = delete;
		CSpatialAudioRenderer &operator=(const CSpatialAudioRenderer &) = delete;

		bool Initialize();
		void Finalize();
		bool IsInitialized() const noexcept { return m_Stream != nullptr; }

	// AudioDecoderFilter::SampleCallback
		void OnSamples(short *pData, size_t Length, int Channels) override;

	// CSpatialAudioRenderer
		void SetVolume(float Volume);

	private:
		/** 入力として扱うチャンネル数 (22.2ch) */
		static constexpr int MAX_CHANNELS = 24;
		/** 静的オブジェクト (BED) の数 */
		static constexpr int STATIC_OBJECT_COUNT = 12;
		/** 位置付きの動的オブジェクトの数 */
		static constexpr int DYNAMIC_OBJECT_COUNT = 11;

		bool SelectObjectFormat(ISpatialAudioClient *pClient, std::vector<BYTE> *pFormat);
		bool ActivateObjects();
		bool ResetStream();
		bool ExpandSamples(const short *pData, size_t Frames, int Channels);
		void PushFrames(const short *pSrc, size_t Frames);
		size_t PopFrames(short *pDst, size_t Frames);
		void ClearQueue();
		bool WriteObject(ISpatialAudioObject *pObject, size_t Frames, int Channel);
		bool WriteLFEObject(ISpatialAudioObject *pObject, size_t Frames);
		bool WriteBuffer(ISpatialAudioObject *pObject, size_t Frames);
		void RenderThreadProc();

		Microsoft::WRL::ComPtr<ISpatialAudioObjectRenderStream> m_Stream;
		Microsoft::WRL::ComPtr<ISpatialAudioObject> m_StaticObjects[STATIC_OBJECT_COUNT];
		Microsoft::WRL::ComPtr<ISpatialAudioObject> m_DynamicObjects[DYNAMIC_OBJECT_COUNT];
		HANDLE m_hBufferEvent = nullptr;
		std::thread m_RenderThread;
		std::atomic<bool> m_Stop{false};
		std::atomic<float> m_Volume{1.0f};
		bool m_ObjectsActivated = false;
		bool m_FloatFormat = false;

		/** OnSamples() で 24ch へ展開する作業バッファ (呼び出し毎に確保しない) */
		std::vector<short> m_ExpandBuffer;
		/** 1 回の更新で取り出した 24ch インターリーブ PCM */
		std::vector<short> m_FrameBuffer;
		/** オブジェクトへ書き込む 1 ch 分のバッファ */
		std::vector<float> m_ObjectBuffer;

		std::mutex m_QueueLock;
		/** 24ch インターリーブのリングバッファ */
		std::vector<short> m_Queue;
		size_t m_QueueCapacity = 0;
		size_t m_QueueRead = 0;
		size_t m_QueueFrames = 0;
	};

}	// namespace TVTest


#endif	// ifndef TVTEST_SPATIAL_AUDIO_RENDERER_H
