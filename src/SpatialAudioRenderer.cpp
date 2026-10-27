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
 @file   SpatialAudioRenderer.cpp
 @brief  Windows 空間オーディオによる 22.2ch 音声の再生
 @author DBCTRADO
*/


#include "stdafx.h"
#include "TVTest.h"
#include "SpatialAudioRenderer.h"
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <propvarutil.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "Common/DebugDef.h"

#pragma comment(lib, "Mmdevapi.lib")


namespace TVTest
{


namespace
{

/*
	22.2ch 音声のチャンネル順

	AudioDecoderFilter は 22.2ch をダウンミックスせず、デコーダから
	受け取った順序のまま 24ch インターリーブ PCM として渡してくる。
	その順序は FFmpeg の AV_CHANNEL_LAYOUT_22POINT2 のチャンネル順
	(チャンネルマスクのビット昇順) であり、以下の通りである。

	  0 FL    1 FR    2 FC    3 LFE1   4 BL    5 BR    6 FLc   7 FRc
	  8 BC    9 SiL  10 SiR  11 TpC   12 TpFL 13 TpFC 14 TpFR 15 TpBL
	 16 TpBC 17 TpBR 18 LFE2 19 TpSiL 20 TpSiR 21 BtFC 22 BtFL 23 BtFR

	※ これは AAC の channel_configuration = 13 が規定する伝送順とは
	   異なる。FFmpeg が自身のレイアウト順へ並べた結果を使っているため、
	   デコーダを FFmpeg 以外へ変更する場合はこの表を見直す必要がある。
*/
enum {
	CHANNEL_FL,
	CHANNEL_FR,
	CHANNEL_FC,
	CHANNEL_LFE1,
	CHANNEL_BL,
	CHANNEL_BR,
	CHANNEL_FLC,
	CHANNEL_FRC,
	CHANNEL_BC,
	CHANNEL_SIL,
	CHANNEL_SIR,
	CHANNEL_TPC,
	CHANNEL_TPFL,
	CHANNEL_TPFC,
	CHANNEL_TPFR,
	CHANNEL_TPBL,
	CHANNEL_TPBC,
	CHANNEL_TPBR,
	CHANNEL_LFE2,
	CHANNEL_TPSIL,
	CHANNEL_TPSIR,
	CHANNEL_BTFC,
	CHANNEL_BTFL,
	CHANNEL_BTFR
};

/*
	静的オブジェクト (BED) に割り当てるチャンネル

	BED の位置は Windows 側で決まっているため、22.2ch のうち位置が
	近いものを割り当てる。前方左右には FL/FR (方位 ±60°) ではなく
	FLc/FRc (±30°) を割り当てている。BED の FrontLeft/FrontRight は
	±30° 前後を想定した位置であり、FL/FR は動的オブジェクトとして
	本来の ±60° に置いた方が定位が合うためである。

	LowFrequency には LFE1 と LFE2 を合成して出力する (下記 WriteLFEObject)。
*/
struct StaticObjectInfo {
	AudioObjectType Type;
	int Channel;
};

constexpr StaticObjectInfo StaticObjects[] = {
	{AudioObjectType_FrontLeft,     CHANNEL_FLC},
	{AudioObjectType_FrontRight,    CHANNEL_FRC},
	{AudioObjectType_FrontCenter,   CHANNEL_FC},
	{AudioObjectType_LowFrequency,  CHANNEL_LFE1},	// LFE2 と合成する
	{AudioObjectType_SideLeft,      CHANNEL_SIL},
	{AudioObjectType_SideRight,     CHANNEL_SIR},
	{AudioObjectType_BackLeft,      CHANNEL_BL},
	{AudioObjectType_BackRight,     CHANNEL_BR},
	{AudioObjectType_TopFrontLeft,  CHANNEL_TPFL},
	{AudioObjectType_TopFrontRight, CHANNEL_TPFR},
	{AudioObjectType_TopBackLeft,   CHANNEL_TPBL},
	{AudioObjectType_TopBackRight,  CHANNEL_TPBR},
};

/*
	動的オブジェクトに割り当てるチャンネルと、その方向

	方位角・仰角は ITU-R BS.2051 の System H (22.2ch)、および
	ISO/IEC 23091-3 のラウドスピーカー配置に基づく。方位角は正面を 0°
	として左回りを正、仰角は上を正とする。上層は +30°、下層は -30°、
	TpC (天井中央) のみ仰角 +90°。
*/
struct DynamicObjectInfo {
	int Channel;
	double Azimuth;
	double Elevation;
};

constexpr DynamicObjectInfo DynamicObjects[] = {
	{CHANNEL_FL,     60.0,   0.0},
	{CHANNEL_FR,    -60.0,   0.0},
	{CHANNEL_BC,    180.0,   0.0},
	{CHANNEL_TPFC,    0.0,  30.0},
	{CHANNEL_TPC,     0.0,  90.0},
	{CHANNEL_TPSIL,  90.0,  30.0},
	{CHANNEL_TPSIR, -90.0,  30.0},
	{CHANNEL_TPBC,  180.0,  30.0},
	{CHANNEL_BTFL,   45.0, -30.0},
	{CHANNEL_BTFR,  -45.0, -30.0},
	{CHANNEL_BTFC,    0.0, -30.0},
};

/** オブジェクトを配置する距離 (メートル) */
constexpr float OBJECT_DISTANCE = 1.5f;

/**
 方位角・仰角を ISpatialAudioObject::SetPosition() の座標へ変換する

 ここでは +X = 右、+Y = 上、+Z = 正面 を仮定している。SetPosition() が
 実際にどの向きを正とするかは公式ドキュメントに明記されていないため、
 実機で 1 チャンネルだけ鳴らして左右・上下・前後が合っているか確認し、
 合っていなければこの関数内の符号を反転する。
*/
void AzimuthElevationToPosition(double Azimuth, double Elevation, float *pX, float *pY, float *pZ)
{
	constexpr double PI = 3.14159265358979323846;
	const double Az = Azimuth * PI / 180.0;
	const double El = Elevation * PI / 180.0;

	*pX = static_cast<float>(-std::sin(Az) * std::cos(El)) * OBJECT_DISTANCE;
	*pY = static_cast<float>(std::sin(El)) * OBJECT_DISTANCE;
	*pZ = static_cast<float>(std::cos(Az) * std::cos(El)) * OBJECT_DISTANCE;
}

/** LFE1 と LFE2 を合成する際のゲイン (-3dB) */
constexpr float LFE_GAIN = 0.70794578f;

/** リングバッファに貯める長さ (大きくすると音切れに強いが遅延が増える) */
constexpr size_t QUEUE_DURATION_MSEC = 200;

/** 空間オーディオのサンプリング周波数 (Windows の空間オーディオは 48kHz 固定) */
constexpr DWORD SAMPLES_PER_SEC = 48000;

/**
 2ch / 5.1ch 入力を 24ch のどのスロットへ入れるか

 BED へ素直に流れるように配置する。5.1ch の順序は LibISDB の
 チャンネルマップ (FL, FR, FC, LFE, BL, BR) に合わせる。
*/
constexpr int MonoChannelMap[] = {CHANNEL_FC};
constexpr int StereoChannelMap[] = {CHANNEL_FLC, CHANNEL_FRC};
constexpr int SurroundChannelMap[] = {
	CHANNEL_FLC, CHANNEL_FRC, CHANNEL_FC, CHANNEL_LFE1, CHANNEL_BL, CHANNEL_BR
};

bool IsSupportedObjectFormat(const WAVEFORMATEX *pFormat, bool *pFloatFormat)
{
	if ((pFormat == nullptr) || (pFormat->nChannels != 1)
			|| (pFormat->nSamplesPerSec != SAMPLES_PER_SEC))
		return false;

	WORD FormatTag = pFormat->wFormatTag;
	if (FormatTag == WAVE_FORMAT_EXTENSIBLE) {
		if (pFormat->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			return false;
		const WAVEFORMATEXTENSIBLE *pExtensible =
			reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(pFormat);
		if (pExtensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
			FormatTag = WAVE_FORMAT_PCM;
		else if (pExtensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
			FormatTag = WAVE_FORMAT_IEEE_FLOAT;
		else
			return false;
	}

	if ((FormatTag == WAVE_FORMAT_PCM) && (pFormat->wBitsPerSample == 16)) {
		*pFloatFormat = false;
		return true;
	}
	if ((FormatTag == WAVE_FORMAT_IEEE_FLOAT) && (pFormat->wBitsPerSample == 32)) {
		*pFloatFormat = true;
		return true;
	}

	return false;
}

}	// namespace


CSpatialAudioRenderer::~CSpatialAudioRenderer()
{
	Finalize();
}


void CSpatialAudioRenderer::SetVolume(float Volume)
{
	m_Volume.store(std::clamp(Volume, 0.0f, 1.0f));
}


bool CSpatialAudioRenderer::Initialize()
{
	Finalize();

	Microsoft::WRL::ComPtr<IMMDeviceEnumerator> Enumerator;
	if (FAILED(::CoCreateInstance(
				__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(&Enumerator))))
		return false;

	Microsoft::WRL::ComPtr<IMMDevice> Device;
	if (FAILED(Enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &Device)))
		return false;

	Microsoft::WRL::ComPtr<ISpatialAudioClient> Client;
	if (FAILED(Device->Activate(
				__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr,
				reinterpret_cast<void **>(Client.GetAddressOf())))
			|| !Client) {
		// 空間オーディオが有効になっていない
		return false;
	}

	std::vector<BYTE> FormatBuffer;
	if (!SelectObjectFormat(Client.Get(), &FormatBuffer))
		return false;

	// BED として使うチャンネルがすべて使えることを確認する
	AudioObjectType StaticMask = AudioObjectType_None;
	for (const StaticObjectInfo &Info : StaticObjects)
		StaticMask = static_cast<AudioObjectType>(StaticMask | Info.Type);

	AudioObjectType NativeMask = AudioObjectType_None;
	if (FAILED(Client->GetNativeStaticObjectTypeMask(&NativeMask))
			|| ((NativeMask & StaticMask) != StaticMask)) {
		TRACE(TEXT("空間オーディオの静的オブジェクトが足りません\n"));
		return false;
	}

	UINT32 MaxDynamicObjects = 0;
	if (FAILED(Client->GetMaxDynamicObjectCount(&MaxDynamicObjects))
			|| (MaxDynamicObjects < DYNAMIC_OBJECT_COUNT)) {
		TRACE(TEXT("空間オーディオの動的オブジェクトが足りません ({} < {})\n"),
			  MaxDynamicObjects, DYNAMIC_OBJECT_COUNT);
		return false;
	}

	m_hBufferEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (m_hBufferEvent == nullptr)
		return false;

	SpatialAudioObjectRenderStreamActivationParams Params = {};
	Params.ObjectFormat = reinterpret_cast<WAVEFORMATEX *>(FormatBuffer.data());
	Params.StaticObjectTypeMask = StaticMask;
	Params.MinDynamicObjectCount = DYNAMIC_OBJECT_COUNT;
	Params.MaxDynamicObjectCount = DYNAMIC_OBJECT_COUNT;
	Params.Category = AudioCategory_Media;
	Params.EventHandle = m_hBufferEvent;

	PROPVARIANT Activation;
	::PropVariantInit(&Activation);
	Activation.vt = VT_BLOB;
	Activation.blob.cbSize = sizeof(Params);
	Activation.blob.pBlobData = reinterpret_cast<BYTE *>(&Params);
	const HRESULT hr = Client->ActivateSpatialAudioStream(
		&Activation, IID_PPV_ARGS(m_Stream.GetAddressOf()));
	// blob はスタック上の Params を指しているだけなので、PropVariantClear()
	// に解放させない
	Activation.vt = VT_EMPTY;
	Activation.blob.cbSize = 0;
	Activation.blob.pBlobData = nullptr;
	::PropVariantClear(&Activation);

	if (FAILED(hr) || !m_Stream) {
		Finalize();
		return false;
	}

	if (FAILED(m_Stream->Start())) {
		Finalize();
		return false;
	}

	m_QueueCapacity = SAMPLES_PER_SEC * QUEUE_DURATION_MSEC / 1000;
	m_Queue.assign(m_QueueCapacity * MAX_CHANNELS, 0);
	m_QueueRead = 0;
	m_QueueFrames = 0;
	m_ObjectsActivated = false;
	m_Stop = false;

	m_RenderThread = std::thread(&CSpatialAudioRenderer::RenderThreadProc, this);

	return true;
}


void CSpatialAudioRenderer::Finalize()
{
	m_Stop = true;
	if (m_hBufferEvent != nullptr)
		::SetEvent(m_hBufferEvent);
	if (m_RenderThread.joinable())
		m_RenderThread.join();

	if (m_Stream)
		m_Stream->Stop();
	for (auto &Object : m_StaticObjects)
		Object.Reset();
	for (auto &Object : m_DynamicObjects)
		Object.Reset();
	m_Stream.Reset();
	m_ObjectsActivated = false;

	if (m_hBufferEvent != nullptr) {
		::CloseHandle(m_hBufferEvent);
		m_hBufferEvent = nullptr;
	}

	ClearQueue();
	m_Queue.clear();
	m_QueueCapacity = 0;
}


bool CSpatialAudioRenderer::SelectObjectFormat(
	ISpatialAudioClient *pClient, std::vector<BYTE> *pFormat)
{
	Microsoft::WRL::ComPtr<IAudioFormatEnumerator> FormatEnumerator;

	if (SUCCEEDED(pClient->GetSupportedAudioObjectFormatEnumerator(&FormatEnumerator))) {
		UINT32 Count = 0;
		if (FAILED(FormatEnumerator->GetCount(&Count)))
			Count = 0;
		for (UINT32 i = 0; i < Count; i++) {
			WAVEFORMATEX *pWaveFormat = nullptr;
			if (FAILED(FormatEnumerator->GetFormat(i, &pWaveFormat)) || (pWaveFormat == nullptr))
				continue;
			bool FloatFormat = false;
			if (IsSupportedObjectFormat(pWaveFormat, &FloatFormat)) {
				const size_t Size = sizeof(WAVEFORMATEX) + pWaveFormat->cbSize;
				pFormat->resize(Size);
				std::memcpy(pFormat->data(), pWaveFormat, Size);
				m_FloatFormat = FloatFormat;
				::CoTaskMemFree(pWaveFormat);
				return true;
			}
			::CoTaskMemFree(pWaveFormat);
		}
	}

	// 列挙できない場合は 16bit PCM を試す
	WAVEFORMATEX WaveFormat = {};
	WaveFormat.wFormatTag = WAVE_FORMAT_PCM;
	WaveFormat.nChannels = 1;
	WaveFormat.nSamplesPerSec = SAMPLES_PER_SEC;
	WaveFormat.wBitsPerSample = 16;
	WaveFormat.nBlockAlign = WaveFormat.nChannels * WaveFormat.wBitsPerSample / 8;
	WaveFormat.nAvgBytesPerSec = WaveFormat.nSamplesPerSec * WaveFormat.nBlockAlign;

	if (FAILED(pClient->IsAudioObjectFormatSupported(&WaveFormat)))
		return false;

	pFormat->resize(sizeof(WaveFormat));
	std::memcpy(pFormat->data(), &WaveFormat, sizeof(WaveFormat));
	m_FloatFormat = false;

	return true;
}


bool CSpatialAudioRenderer::ActivateObjects()
{
	for (int i = 0; i < STATIC_OBJECT_COUNT; i++) {
		if (FAILED(m_Stream->ActivateSpatialAudioObject(
					StaticObjects[i].Type, &m_StaticObjects[i])))
			return false;
	}

	for (int i = 0; i < DYNAMIC_OBJECT_COUNT; i++) {
		if (FAILED(m_Stream->ActivateSpatialAudioObject(
					AudioObjectType_Dynamic, &m_DynamicObjects[i])))
			return false;
		float x, y, z;
		AzimuthElevationToPosition(
			DynamicObjects[i].Azimuth, DynamicObjects[i].Elevation, &x, &y, &z);
		if (FAILED(m_DynamicObjects[i]->SetPosition(x, y, z)))
			return false;
	}

	m_ObjectsActivated = true;

	return true;
}


bool CSpatialAudioRenderer::ResetStream()
{
	if (!m_Stream)
		return false;

	const HRESULT hr = m_Stream->Reset();

	for (auto &Object : m_StaticObjects)
		Object.Reset();
	for (auto &Object : m_DynamicObjects)
		Object.Reset();
	m_ObjectsActivated = false;

	ClearQueue();

	return SUCCEEDED(hr);
}


void CSpatialAudioRenderer::OnSamples(short *pData, size_t Length, int Channels)
{
	if ((pData == nullptr) || (Length == 0) || m_Stop)
		return;

	if (Channels == MAX_CHANNELS) {
		PushFrames(pData, Length);
	} else {
		if (!ExpandSamples(pData, Length, Channels))
			return;
		PushFrames(m_ExpandBuffer.data(), Length);
	}
}


bool CSpatialAudioRenderer::ExpandSamples(const short *pData, size_t Frames, int Channels)
{
	const int *pMap;
	size_t MapSize;

	switch (Channels) {
	case 1:
		pMap = MonoChannelMap;
		MapSize = lengthof(MonoChannelMap);
		break;
	case 2:
		pMap = StereoChannelMap;
		MapSize = lengthof(StereoChannelMap);
		break;
	case 6:
		pMap = SurroundChannelMap;
		MapSize = lengthof(SurroundChannelMap);
		break;
	default:
		return false;
	}

	const size_t Size = Frames * MAX_CHANNELS;
	if (m_ExpandBuffer.size() < Size)
		m_ExpandBuffer.resize(Size);
	std::fill_n(m_ExpandBuffer.begin(), Size, static_cast<short>(0));

	for (size_t i = 0; i < Frames; i++) {
		const short *pSrc = &pData[i * Channels];
		short *pDst = &m_ExpandBuffer[i * MAX_CHANNELS];
		for (size_t j = 0; j < MapSize; j++)
			pDst[pMap[j]] = pSrc[j];
	}

	return true;
}


void CSpatialAudioRenderer::PushFrames(const short *pSrc, size_t Frames)
{
	std::lock_guard<std::mutex> Lock(m_QueueLock);

	if (m_QueueCapacity == 0)
		return;

	// 入力が一度に容量を超える場合は、新しい側を残す
	if (Frames > m_QueueCapacity) {
		pSrc += (Frames - m_QueueCapacity) * MAX_CHANNELS;
		Frames = m_QueueCapacity;
	}

	// 溢れる分は古い側を捨てる
	if (m_QueueFrames + Frames > m_QueueCapacity) {
		const size_t Drop = m_QueueFrames + Frames - m_QueueCapacity;
		m_QueueRead = (m_QueueRead + Drop) % m_QueueCapacity;
		m_QueueFrames -= Drop;
	}

	size_t Pos = (m_QueueRead + m_QueueFrames) % m_QueueCapacity;
	size_t Remain = Frames;
	while (Remain > 0) {
		const size_t Count = std::min(Remain, m_QueueCapacity - Pos);
		std::memcpy(
			&m_Queue[Pos * MAX_CHANNELS], pSrc,
			Count * MAX_CHANNELS * sizeof(short));
		pSrc += Count * MAX_CHANNELS;
		Pos = (Pos + Count) % m_QueueCapacity;
		Remain -= Count;
	}

	m_QueueFrames += Frames;
}


size_t CSpatialAudioRenderer::PopFrames(short *pDst, size_t Frames)
{
	std::lock_guard<std::mutex> Lock(m_QueueLock);

	if (m_QueueCapacity == 0)
		return 0;

	size_t Remain = std::min(Frames, m_QueueFrames);
	const size_t Popped = Remain;
	while (Remain > 0) {
		const size_t Count = std::min(Remain, m_QueueCapacity - m_QueueRead);
		std::memcpy(
			pDst, &m_Queue[m_QueueRead * MAX_CHANNELS],
			Count * MAX_CHANNELS * sizeof(short));
		pDst += Count * MAX_CHANNELS;
		m_QueueRead = (m_QueueRead + Count) % m_QueueCapacity;
		Remain -= Count;
	}

	m_QueueFrames -= Popped;

	return Popped;
}


void CSpatialAudioRenderer::ClearQueue()
{
	std::lock_guard<std::mutex> Lock(m_QueueLock);

	m_QueueRead = 0;
	m_QueueFrames = 0;
}


bool CSpatialAudioRenderer::WriteObject(
	ISpatialAudioObject *pObject, size_t Frames, int Channel)
{
	if ((pObject == nullptr) || (Channel < 0) || (Channel >= MAX_CHANNELS))
		return false;

	const float Volume = m_Volume.load();

	m_ObjectBuffer.resize(Frames);
	for (size_t i = 0; i < Frames; i++) {
		m_ObjectBuffer[i] =
			static_cast<float>(m_FrameBuffer[i * MAX_CHANNELS + Channel]) * Volume;
	}

	return WriteBuffer(pObject, Frames);
}


bool CSpatialAudioRenderer::WriteLFEObject(ISpatialAudioObject *pObject, size_t Frames)
{
	if (pObject == nullptr)
		return false;

	// LFE1 と LFE2 は無指向性なので、合成して 1 つの LowFrequency へ出力する
	const float Volume = m_Volume.load() * LFE_GAIN;

	m_ObjectBuffer.resize(Frames);
	for (size_t i = 0; i < Frames; i++) {
		const short *pFrame = &m_FrameBuffer[i * MAX_CHANNELS];
		m_ObjectBuffer[i] =
			(static_cast<float>(pFrame[CHANNEL_LFE1])
				+ static_cast<float>(pFrame[CHANNEL_LFE2])) * Volume;
	}

	return WriteBuffer(pObject, Frames);
}


bool CSpatialAudioRenderer::WriteBuffer(ISpatialAudioObject *pObject, size_t Frames)
{
	BYTE *pBuffer = nullptr;
	UINT32 BufferSize = 0;

	if (FAILED(pObject->GetBuffer(&pBuffer, &BufferSize)) || (pBuffer == nullptr))
		return false;

	const size_t SampleSize = m_FloatFormat ? sizeof(float) : sizeof(short);
	if (BufferSize / SampleSize < Frames)
		return false;

	if (m_FloatFormat) {
		float *pOut = reinterpret_cast<float *>(pBuffer);
		for (size_t i = 0; i < Frames; i++)
			pOut[i] = std::clamp(m_ObjectBuffer[i] / 32768.0f, -1.0f, 1.0f);
	} else {
		short *pOut = reinterpret_cast<short *>(pBuffer);
		for (size_t i = 0; i < Frames; i++) {
			pOut[i] = static_cast<short>(
				std::clamp(m_ObjectBuffer[i], -32768.0f, 32767.0f));
		}
	}

	return true;
}


void CSpatialAudioRenderer::RenderThreadProc()
{
	::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	unsigned int TimeoutCount = 0;
	unsigned int WriteErrorCount = 0;

	while (!m_Stop) {
		if (::WaitForSingleObject(m_hBufferEvent, 100) != WAIT_OBJECT_0) {
			// バッファ完了イベントが来ない状態が続く場合は、デバイスが
			// 切り替わった等と考えてストリームを作り直す
			if (++TimeoutCount >= 10) {
				ResetStream();
				TimeoutCount = 0;
			}
			continue;
		}
		TimeoutCount = 0;

		UINT32 AvailableObjects = 0;
		UINT32 Frames = 0;
		if (FAILED(m_Stream->BeginUpdatingAudioObjects(&AvailableObjects, &Frames))) {
			ResetStream();
			continue;
		}

		if (AvailableObjects < DYNAMIC_OBJECT_COUNT) {
			m_Stream->EndUpdatingAudioObjects();
			ResetStream();
			continue;
		}

		if (!m_ObjectsActivated && !ActivateObjects()) {
			m_Stream->EndUpdatingAudioObjects();
			ResetStream();
			continue;
		}

		// 取り出せなかった分は無音にする
		const size_t Size = static_cast<size_t>(Frames) * MAX_CHANNELS;
		if (m_FrameBuffer.size() < Size)
			m_FrameBuffer.resize(Size);
		const size_t Popped = PopFrames(m_FrameBuffer.data(), Frames);
		if (Popped < Frames) {
			std::fill_n(
				m_FrameBuffer.begin() + Popped * MAX_CHANNELS,
				(Frames - Popped) * MAX_CHANNELS, static_cast<short>(0));
		}

		bool WriteOK = true;
		for (int i = 0; i < STATIC_OBJECT_COUNT; i++) {
			const bool OK = (StaticObjects[i].Type == AudioObjectType_LowFrequency) ?
				WriteLFEObject(m_StaticObjects[i].Get(), Frames) :
				WriteObject(m_StaticObjects[i].Get(), Frames, StaticObjects[i].Channel);
			WriteOK = OK && WriteOK;
		}
		for (int i = 0; i < DYNAMIC_OBJECT_COUNT; i++) {
			WriteOK = WriteObject(
				m_DynamicObjects[i].Get(), Frames, DynamicObjects[i].Channel) && WriteOK;
		}

		m_Stream->EndUpdatingAudioObjects();

		if (WriteOK) {
			WriteErrorCount = 0;
		} else if (++WriteErrorCount >= 3) {
			// 一時的な書き込み失敗でリセットするとクリック音が出るため、
			// 連続して失敗した場合だけ作り直す
			ResetStream();
			WriteErrorCount = 0;
		}
	}

	::CoUninitialize();
}


}	// namespace TVTest
