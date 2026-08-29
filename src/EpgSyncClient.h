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


#ifndef TVTEST_EPG_SYNC_CLIENT_H
#define TVTEST_EPG_SYNC_CLIENT_H


#include <memory>
#include "StringUtility.h"
#include "LibISDB/LibISDB/EPG/EPGDatabase.hpp"


namespace TVTest
{

	/** EPG 共有サーバのクライアント

	LAN 上の epgsync サーバを介して、複数の TVTest 間で EPG をサービス単位で
	融通しあう。

	- 起動時にサーバの持ち物と突き合わせ、自分より新しいサービスを取り込む
	- 自分が EIT から取得したサービスをサーバへ送る
	- 他の機が更新したらサーバからの通知を受けて取り込む

	番組情報のシリアライズは LibISDB::EPGDataSerializer が行う。
	サーバは内容を解釈せず、バイト列として保管するだけ。
	*/
	class CEpgSyncClient
	{
	public:
		struct SyncSettings {
			bool fEnable = false;
			String Server;                  /**< http://host:port */
			String Token;                   /**< 認証トークン(空なら認証なし) */
			String Name;                    /**< この機の名前(サーバのログに出る) */
			DWORD ScanInterval = 60 * 1000; /**< 送信対象を探す間隔(ミリ秒) */

			bool IsValid() const { return fEnable && !Server.empty(); }

			bool operator == (const SyncSettings &rhs) const = default;
		};

		class ABSTRACT_CLASS(CEventHandler)
		{
		public:
			virtual ~CEventHandler() = default;

			/** サービスの番組情報を取り込んだ

			ワーカースレッドから呼ばれる。UI を触る場合は PostMessage すること。
			*/
			virtual void OnServiceMerged(WORD NetworkID, WORD TransportStreamID, WORD ServiceID) {}
		};

		CEpgSyncClient();
		~CEpgSyncClient();

		CEpgSyncClient(const CEpgSyncClient &) = delete;
		CEpgSyncClient &operator=(const CEpgSyncClient &) = delete;

		bool Open(LibISDB::EPGDatabase *pEPGDatabase, const SyncSettings &Settings);
		void Close();
		bool IsOpen() const;

		void SetEventHandler(CEventHandler *pEventHandler);

		/** 送信対象の探索を促す(EPG 取得完了時など) */
		void RequestSend();

		const SyncSettings &GetSettings() const { return m_Settings; }

	private:
		class CImpl;

		std::unique_ptr<CImpl> m_Impl;
		SyncSettings m_Settings;
		CEventHandler *m_pEventHandler = nullptr;
	};

} // namespace TVTest


#endif // TVTEST_EPG_SYNC_CLIENT_H
