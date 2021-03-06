/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "threads/SystemClock.h"
#include "MusicInfoScanner.h"
#include "music/tags/MusicInfoTagLoaderFactory.h"
#include "MusicAlbumInfo.h"
#include "MusicInfoScraper.h"
#include "filesystem/MusicDatabaseDirectory.h"
#include "filesystem/MusicDatabaseDirectory/DirectoryNode.h"
#include "Util.h"
#include "utils/md5.h"
#include "GUIInfoManager.h"
#include "utils/Variant.h"
#include "NfoFile.h"
#include "music/tags/MusicInfoTag.h"
#include "guilib/GUIWindowManager.h"
#include "dialogs/GUIDialogExtendedProgressBar.h"
#include "dialogs/GUIDialogProgress.h"
#include "dialogs/GUIDialogSelect.h"
#include "guilib/GUIKeyboardFactory.h"
#include "filesystem/File.h"
#include "filesystem/Directory.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "FileItem.h"
#include "guilib/LocalizeStrings.h"
#include "utils/StringUtils.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "utils/URIUtils.h"
#include "TextureCache.h"
#include "music/MusicThumbLoader.h"
#include "interfaces/AnnouncementManager.h"
#include "GUIUserMessages.h"
#include "addons/AddonManager.h"
#include "addons/Scraper.h"

#include <algorithm>

using namespace std;
using namespace MUSIC_INFO;
using namespace XFILE;
using namespace MUSICDATABASEDIRECTORY;
using namespace MUSIC_GRABBER;
using namespace ADDON;

CMusicInfoScanner::CMusicInfoScanner() : CThread("MusicInfoScanner"), m_fileCountReader(this, "MusicFileCounter")
{
  m_bRunning = false;
  m_showDialog = false;
  m_handle = NULL;
  m_bCanInterrupt = false;
  m_currentItem=0;
  m_itemCount=0;
  m_flags = 0;
}

CMusicInfoScanner::~CMusicInfoScanner()
{
}

void CMusicInfoScanner::Process()
{
  ANNOUNCEMENT::CAnnouncementManager::Announce(ANNOUNCEMENT::AudioLibrary, "xbmc", "OnScanStarted");
  try
  {
    unsigned int tick = XbmcThreads::SystemClockMillis();

    m_musicDatabase.Open();

    if (m_showDialog && !CSettings::Get().GetBool("musiclibrary.backgroundupdate"))
    {
      CGUIDialogExtendedProgressBar* dialog =
        (CGUIDialogExtendedProgressBar*)g_windowManager.GetWindow(WINDOW_DIALOG_EXT_PROGRESS);
      m_handle = dialog->GetHandle(g_localizeStrings.Get(314));
    }

    m_bCanInterrupt = true;

    if (m_scanType == 0) // load info from files
    {
      CLog::Log(LOGDEBUG, "%s - Starting scan", __FUNCTION__);

      if (m_handle)
        m_handle->SetTitle(g_localizeStrings.Get(505));

      // Reset progress vars
      m_currentItem=0;
      m_itemCount=-1;

      // Create the thread to count all files to be scanned
      SetPriority( GetMinPriority() );
      if (m_handle)
        m_fileCountReader.Create();

      // Database operations should not be canceled
      // using Interupt() while scanning as it could
      // result in unexpected behaviour.
      m_bCanInterrupt = false;
      m_needsCleanup = false;

      bool commit = false;
      bool cancelled = false;
      for (std::set<std::string>::const_iterator it = m_pathsToScan.begin(); it != m_pathsToScan.end(); it++)
      {
        /*
         * A copy of the directory path is used because the path supplied is
         * immediately removed from the m_pathsToScan set in DoScan(). If the
         * reference points to the entry in the set a null reference error
         * occurs.
         */
        if (!DoScan(*it))
          cancelled = true;
        commit = !cancelled;
      }

      if (commit)
      {
        g_infoManager.ResetLibraryBools();

        if (m_needsCleanup)
        {
          if (m_handle)
          {
            m_handle->SetTitle(g_localizeStrings.Get(700));
            m_handle->SetText("");
          }

          m_musicDatabase.CleanupOrphanedItems();

          if (m_handle)
            m_handle->SetTitle(g_localizeStrings.Get(331));

          m_musicDatabase.Compress(false);
        }
      }

      m_fileCountReader.StopThread();

      m_musicDatabase.EmptyCache();
      
      tick = XbmcThreads::SystemClockMillis() - tick;
      CLog::Log(LOGNOTICE, "My Music: Scanning for music info using worker thread, operation took %s", StringUtils::SecondsToTimeString(tick / 1000).c_str());
    }
    if (m_scanType == 1) // load album info
    {
      for (std::set<std::string>::const_iterator it = m_pathsToScan.begin(); it != m_pathsToScan.end(); ++it)
      {
        CQueryParams params;
        CDirectoryNode::GetDatabaseInfo(*it, params);
        if (m_musicDatabase.HasAlbumInfo(params.GetArtistId())) // should this be here?
          continue;

        CAlbum album;
        m_musicDatabase.GetAlbumInfo(params.GetAlbumId(), album, &album.songs);
        if (m_handle)
        {
          float percentage = (float) std::distance(it, m_pathsToScan.end()) / m_pathsToScan.size();
          m_handle->SetText(StringUtils::Join(album.artist, g_advancedSettings.m_musicItemSeparator) + " - " + album.strAlbum);
          m_handle->SetPercentage(percentage);
        }

        CMusicAlbumInfo albumInfo;
        UpdateDatabaseAlbumInfo(*it, albumInfo, false);

        if (m_bStop)
          break;
      }
    }
    if (m_scanType == 2) // load artist info
    {
      for (std::set<std::string>::const_iterator it = m_pathsToScan.begin(); it != m_pathsToScan.end(); ++it)
      {
        CQueryParams params;
        CDirectoryNode::GetDatabaseInfo(*it, params);
        if (m_musicDatabase.HasArtistInfo(params.GetArtistId())) // should this be here?
            continue;

        CArtist artist;
        m_musicDatabase.GetArtistInfo(params.GetArtistId(), artist);
        m_musicDatabase.GetArtistPath(params.GetArtistId(), artist.strPath);

        if (m_handle)
        {
          float percentage = (float) (std::distance(m_pathsToScan.begin(), it) / m_pathsToScan.size()) * 100;
          m_handle->SetText(artist.strArtist);
          m_handle->SetPercentage(percentage);
        }
        
        CMusicArtistInfo artistInfo;
        UpdateDatabaseArtistInfo(*it, artistInfo, false);

        if (m_bStop)
          break;
      }
    }

  }
  catch (...)
  {
    CLog::Log(LOGERROR, "MusicInfoScanner: Exception while scanning.");
  }
  m_musicDatabase.Close();
  CLog::Log(LOGDEBUG, "%s - Finished scan", __FUNCTION__);
  
  m_bRunning = false;
  ANNOUNCEMENT::CAnnouncementManager::Announce(ANNOUNCEMENT::AudioLibrary, "xbmc", "OnScanFinished");
  
  // we need to clear the musicdb cache and update any active lists
  CUtil::DeleteMusicDatabaseDirectoryCache();
  CGUIMessage msg(GUI_MSG_SCAN_FINISHED, 0, 0, 0);
  g_windowManager.SendThreadMessage(msg);
  
  if (m_handle)
    m_handle->MarkFinished();
  m_handle = NULL;
}

void CMusicInfoScanner::Start(const CStdString& strDirectory, int flags)
{
  m_fileCountReader.StopThread();
  StopThread();
  m_pathsToScan.clear();
  m_flags = flags;

  if (strDirectory.IsEmpty())
  { // scan all paths in the database.  We do this by scanning all paths in the db, and crossing them off the list as
    // we go.
    m_musicDatabase.Open();
    m_musicDatabase.GetPaths(m_pathsToScan);
    m_musicDatabase.Close();
  }
  else
    m_pathsToScan.insert(strDirectory);

  m_scanType = 0;
  Create();
  m_bRunning = true;
}

void CMusicInfoScanner::FetchAlbumInfo(const CStdString& strDirectory,
                                       bool refresh)
{
  m_fileCountReader.StopThread();
  StopThread();
  m_pathsToScan.clear();

  CFileItemList items;
  if (strDirectory.IsEmpty())
  {
    m_musicDatabase.Open();
    m_musicDatabase.GetAlbumsNav("musicdb://albums/", items);
    m_musicDatabase.Close();
  }
  else
  {
    if (URIUtils::HasSlashAtEnd(strDirectory)) // directory
      CDirectory::GetDirectory(strDirectory,items);
    else
    {
      CFileItemPtr item(new CFileItem(strDirectory,false));
      items.Add(item);
    }
  }

  m_musicDatabase.Open();
  for (int i=0;i<items.Size();++i)
  {
    if (CMusicDatabaseDirectory::IsAllItem(items[i]->GetPath()) || items[i]->IsParentFolder())
      continue;

    m_pathsToScan.insert(items[i]->GetPath());
    if (refresh)
    {
      m_musicDatabase.DeleteAlbumInfo(items[i]->GetMusicInfoTag()->GetDatabaseId());
    }
  }
  m_musicDatabase.Close();

  m_scanType = 1;
  Create();
  m_bRunning = true;
}

void CMusicInfoScanner::FetchArtistInfo(const CStdString& strDirectory,
                                        bool refresh)
{
  m_fileCountReader.StopThread();
  StopThread();
  m_pathsToScan.clear();
  CFileItemList items;

  if (strDirectory.IsEmpty())
  {
    m_musicDatabase.Open();
    m_musicDatabase.GetArtistsNav("musicdb://artists/", items, false, -1);
    m_musicDatabase.Close();
  }
  else
  {
    if (URIUtils::HasSlashAtEnd(strDirectory)) // directory
      CDirectory::GetDirectory(strDirectory,items);
    else
    {
      CFileItemPtr newItem(new CFileItem(strDirectory,false));
      items.Add(newItem);
    }
  }

  m_musicDatabase.Open();
  for (int i=0;i<items.Size();++i)
  {
    if (CMusicDatabaseDirectory::IsAllItem(items[i]->GetPath()) || items[i]->IsParentFolder())
      continue;

    m_pathsToScan.insert(items[i]->GetPath());
    if (refresh)
    {
      m_musicDatabase.DeleteArtistInfo(items[i]->GetMusicInfoTag()->GetDatabaseId());
    }
  }
  m_musicDatabase.Close();

  m_scanType = 2;
  Create();
  m_bRunning = true;
}

bool CMusicInfoScanner::IsScanning()
{
  return m_bRunning;
}

void CMusicInfoScanner::Stop()
{
  if (m_bCanInterrupt)
    m_musicDatabase.Interupt();

  StopThread(false);
}

static void OnDirectoryScanned(const CStdString& strDirectory)
{
  CGUIMessage msg(GUI_MSG_DIRECTORY_SCANNED, 0, 0, 0);
  msg.SetStringParam(strDirectory);
  g_windowManager.SendThreadMessage(msg);
}

static CStdString Prettify(const CStdString& strDirectory)
{
  CURL url(strDirectory);
  CStdString strStrippedPath = url.GetWithoutUserDetails();
  CURL::Decode(strStrippedPath);

  return strStrippedPath;
}

bool CMusicInfoScanner::DoScan(const CStdString& strDirectory)
{
  if (m_handle)
    m_handle->SetText(Prettify(strDirectory));

  // Discard all excluded files defined by m_musicExcludeRegExps
  CStdStringArray regexps = g_advancedSettings.m_audioExcludeFromScanRegExps;
  if (CUtil::ExcludeFileOrFolder(strDirectory, regexps))
    return true;

  // load subfolder
  CFileItemList items;
  CDirectory::GetDirectory(strDirectory, items, g_advancedSettings.m_musicExtensions + "|.jpg|.tbn|.lrc|.cdg");

  // sort and get the path hash.  Note that we don't filter .cue sheet items here as we want
  // to detect changes in the .cue sheet as well.  The .cue sheet items only need filtering
  // if we have a changed hash.
  items.Sort(SORT_METHOD_LABEL, SortOrderAscending);
  CStdString hash;
  GetPathHash(items, hash);

  // check whether we need to rescan or not
  CStdString dbHash;
  if ((m_flags & SCAN_RESCAN) || !m_musicDatabase.GetPathHash(strDirectory, dbHash) || dbHash != hash)
  { // path has changed - rescan
    if (dbHash.IsEmpty())
      CLog::Log(LOGDEBUG, "%s Scanning dir '%s' as not in the database", __FUNCTION__, strDirectory.c_str());
    else
      CLog::Log(LOGDEBUG, "%s Rescanning dir '%s' due to change", __FUNCTION__, strDirectory.c_str());

    // filter items in the sub dir (for .cue sheet support)
    items.FilterCueItems();
    items.Sort(SORT_METHOD_LABEL, SortOrderAscending);

    // and then scan in the new information
    if (RetrieveMusicInfo(strDirectory, items) > 0)
    {
      if (m_handle)
        OnDirectoryScanned(strDirectory);
    }

    // save information about this folder
    m_musicDatabase.SetPathHash(strDirectory, hash);
  }
  else
  { // path is the same - no need to rescan
    CLog::Log(LOGDEBUG, "%s Skipping dir '%s' due to no change", __FUNCTION__, strDirectory.c_str());
    m_currentItem += CountFiles(items, false);  // false for non-recursive

    // updated the dialog with our progress
    if (m_handle)
    {
      if (m_itemCount>0)
        m_handle->SetPercentage(m_currentItem/(float)m_itemCount*100);
      OnDirectoryScanned(strDirectory);
    }
  }

  // now scan the subfolders
  for (int i = 0; i < items.Size(); ++i)
  {
    CFileItemPtr pItem = items[i];

    if (m_bStop)
      break;
    // if we have a directory item (non-playlist) we then recurse into that folder
    if (pItem->m_bIsFolder && !pItem->IsParentFolder() && !pItem->IsPlayList())
    {
      CStdString strPath=pItem->GetPath();
      if (!DoScan(strPath))
      {
        m_bStop = true;
      }
    }
  }

  return !m_bStop;
}

INFO_RET CMusicInfoScanner::ScanTags(const CFileItemList& items, CFileItemList& scannedItems)
{
  CStdStringArray regexps = g_advancedSettings.m_audioExcludeFromScanRegExps;

  for (int i = 0; i < items.Size(); ++i)
  {
    if (m_bStop)
      return INFO_CANCELLED;

    CFileItemPtr pItem = items[i];

    if (CUtil::ExcludeFileOrFolder(pItem->GetPath(), regexps))
      continue;

    if (pItem->m_bIsFolder || pItem->IsPlayList() || pItem->IsPicture() || pItem->IsLyrics())
      continue;

    m_currentItem++;

    CMusicInfoTag& tag = *pItem->GetMusicInfoTag();
    if (!tag.Loaded())
    {
      auto_ptr<IMusicInfoTagLoader> pLoader (CMusicInfoTagLoaderFactory::CreateLoader(pItem->GetPath()));
      if (NULL != pLoader.get())
        pLoader->Load(pItem->GetPath(), tag);
    }

    if (m_handle && m_itemCount>0)
      m_handle->SetPercentage(m_currentItem/(float)m_itemCount*100);

    if (!tag.Loaded())
    {
      CLog::Log(LOGDEBUG, "%s - No tag found for: %s", __FUNCTION__, pItem->GetPath().c_str());
      continue;
    }
    scannedItems.Add(pItem);
  }
  return INFO_ADDED;
}

void CMusicInfoScanner::FileItemsToAlbums(CFileItemList& items, VECALBUMS& albums, MAPSONGS* songsMap /* = NULL */)
{
  for (int i = 0; i < items.Size(); ++i)
  {
    CFileItemPtr pItem = items[i];
    CMusicInfoTag& tag = *pItem->GetMusicInfoTag();
    CSong song(*pItem);

    // keep the db-only fields intact on rescan...
    if (songsMap != NULL)
    {
      MAPSONGS::iterator it = songsMap->find(pItem->GetPath());
      if (it != songsMap->end())
      {
        song.iTimesPlayed = it->second.iTimesPlayed;
        song.lastPlayed = it->second.lastPlayed;
        song.iKaraokeNumber = it->second.iKaraokeNumber;
        if (song.rating == '0')    song.rating = it->second.rating;
        if (song.strThumb.empty()) song.strThumb = it->second.strThumb;
      }
    }

    if (!tag.GetMusicBrainzArtistID().empty())
    {
      for (vector<string>::const_iterator it = tag.GetMusicBrainzArtistID().begin(); it != tag.GetMusicBrainzArtistID().end(); ++it)
      {
        CStdString strJoinPhrase = (it == --tag.GetMusicBrainzArtistID().end() ? "" : g_advancedSettings.m_musicItemSeparator);
        CArtistCredit mbartist(song.artist[0], *it, strJoinPhrase);
        song.artistCredits.push_back(mbartist);
      }
      song.artist = tag.GetArtist();
    }
    else
    {
      for (vector<string>::const_iterator it = tag.GetArtist().begin(); it != tag.GetArtist().end(); ++it)
      {
        CStdString strJoinPhrase = (it == --tag.GetArtist().end() ? "" : g_advancedSettings.m_musicItemSeparator);
        CArtistCredit nonmbartist(*it, strJoinPhrase);
        song.artistCredits.push_back(nonmbartist);
      }
      song.artist = tag.GetArtist();
    }

    bool found = false;
    for (VECALBUMS::iterator it = albums.begin(); it != albums.end(); ++it)
    {
      if (it->strAlbum == tag.GetAlbum() && it->strMusicBrainzAlbumID == tag.GetMusicBrainzAlbumID())
      {
        it->songs.push_back(song);
        found = true;
      }
    }
    if (!found)
    {
      CAlbum album(*pItem.get());
      if (!tag.GetMusicBrainzAlbumArtistID().empty())
      {
        for (vector<string>::const_iterator it = tag.GetMusicBrainzAlbumArtistID().begin(); it != tag.GetMusicBrainzAlbumArtistID().end(); ++it)
        {
          // Picard always stored the display artist string in the first artist slot, no need to split it
          CStdString strJoinPhrase = (it == --tag.GetMusicBrainzAlbumArtistID().end() ? "" : g_advancedSettings.m_musicItemSeparator);
          CArtistCredit mbartist(tag.GetAlbumArtist()[0], *it, strJoinPhrase);
          album.artistCredits.push_back(mbartist);
        }
        album.artist = tag.GetAlbumArtist();
      }
      else
      {
        for (vector<string>::const_iterator it = tag.GetAlbumArtist().begin(); it != tag.GetAlbumArtist().end(); ++it)
        {
          CStdString strJoinPhrase = (it == --tag.GetAlbumArtist().end() ? "" : g_advancedSettings.m_musicItemSeparator);
          CArtistCredit nonmbartist(*it, strJoinPhrase);
          album.artistCredits.push_back(nonmbartist);
        }
        album.artist = tag.GetAlbumArtist();
      }
      album.songs.push_back(song);
      albums.push_back(album);
    }
  }
}

int CMusicInfoScanner::RetrieveMusicInfo(const CStdString& strDirectory, CFileItemList& items)
{
  MAPSONGS songsMap;

  // get all information for all files in current directory from database, and remove them
  if (m_musicDatabase.RemoveSongsFromPath(strDirectory, songsMap))
    m_needsCleanup = true;

  CFileItemList scannedItems;
  if (ScanTags(items, scannedItems) == INFO_CANCELLED)
    return 0;

  VECALBUMS albums;
  FileItemsToAlbums(scannedItems, albums, &songsMap);
  if (!(m_flags & SCAN_ONLINE))
    FixupAlbums(albums);
  FindArtForAlbums(albums, items.GetPath());

  int numAdded = 0;
  ADDON::AddonPtr addon;
  ADDON::ScraperPtr albumScraper;
  ADDON::ScraperPtr artistScraper;
  if(ADDON::CAddonMgr::Get().GetDefault(ADDON::ADDON_SCRAPER_ALBUMS, addon))
    albumScraper = boost::dynamic_pointer_cast<ADDON::CScraper>(addon);

  if(ADDON::CAddonMgr::Get().GetDefault(ADDON::ADDON_SCRAPER_ARTISTS, addon))
    artistScraper = boost::dynamic_pointer_cast<ADDON::CScraper>(addon);

  // Add each album
  for (VECALBUMS::iterator album = albums.begin(); album != albums.end(); ++album)
  {
    if (m_bStop)
      break;

    album->strPath = strDirectory;
    m_musicDatabase.BeginTransaction();

    // Check if the album has already been downloaded or failed
    map<CAlbum, CAlbum>::iterator cachedAlbum = m_albumCache.find(*album);
    if (cachedAlbum == m_albumCache.end())
    {
      // No - download the information
      CMusicAlbumInfo albumInfo;
      INFO_RET albumDownloadStatus = INFO_NOT_FOUND;
      if (m_flags & SCAN_ONLINE)
        albumDownloadStatus = DownloadAlbumInfo(*album, albumScraper, albumInfo);

      if (albumDownloadStatus == INFO_ADDED || albumDownloadStatus == INFO_HAVE_ALREADY)
      {
        CAlbum &downloadedAlbum = albumInfo.GetAlbum();
        downloadedAlbum.idAlbum = m_musicDatabase.AddAlbum(downloadedAlbum.strAlbum,
                                                           downloadedAlbum.strMusicBrainzAlbumID,
                                                           downloadedAlbum.GetArtistString(),
                                                           downloadedAlbum.GetGenreString(),
                                                           downloadedAlbum.iYear,
                                                           downloadedAlbum.bCompilation);
        m_musicDatabase.SetAlbumInfo(downloadedAlbum.idAlbum,
                                     downloadedAlbum,
                                     downloadedAlbum.songs);
        m_musicDatabase.SetArtForItem(downloadedAlbum.idAlbum,
                                      "album", album->art);
        GetAlbumArtwork(downloadedAlbum.idAlbum, downloadedAlbum);
        m_albumCache.insert(make_pair(*album, albumInfo.GetAlbum()));
      }
      else if (albumDownloadStatus == INFO_CANCELLED)
        break;
      else // Cache the lookup failure so we don't retry
      {
        album->idAlbum = m_musicDatabase.AddAlbum(album->strAlbum,
                                                  album->strMusicBrainzAlbumID,
                                                  album->GetArtistString(),
                                                  album->GetGenreString(),
                                                  album->iYear,
                                                  album->bCompilation);
        m_albumCache.insert(make_pair(*album, *album));
      }

      // Update the cache pointer with our newly created info
      cachedAlbum = m_albumCache.find(*album);
    }

    if (m_bStop)
      break;

    // Add the album artists
    for (VECARTISTCREDITS::iterator artistCredit = cachedAlbum->second.artistCredits.begin(); artistCredit != cachedAlbum->second.artistCredits.end(); ++artistCredit)
    {
      if (m_bStop)
        break;

      // Check if the artist has already been downloaded or failed
      map<CArtistCredit, CArtist>::iterator cachedArtist = m_artistCache.find(*artistCredit);
      if (cachedArtist == m_artistCache.end())
      {
        CArtist artistTmp;
        artistTmp.strArtist = artistCredit->GetArtist();
        artistTmp.strMusicBrainzArtistID = artistCredit->GetMusicBrainzArtistID();
        URIUtils::GetParentPath(album->strPath, artistTmp.strPath);

        // No - download the information
        CMusicArtistInfo artistInfo;
        INFO_RET artistDownloadStatus = INFO_NOT_FOUND;
        if (m_flags & SCAN_ONLINE)
          artistDownloadStatus = DownloadArtistInfo(artistTmp, artistScraper, artistInfo);

        if (artistDownloadStatus == INFO_ADDED || artistDownloadStatus == INFO_HAVE_ALREADY)
        {
          CArtist &downloadedArtist = artistInfo.GetArtist();
          downloadedArtist.idArtist = m_musicDatabase.AddArtist(downloadedArtist.strArtist,
                                                                downloadedArtist.strMusicBrainzArtistID);
          m_musicDatabase.SetArtistInfo(downloadedArtist.idArtist,
                                        downloadedArtist);

          URIUtils::GetParentPath(album->strPath, downloadedArtist.strPath);
          map<string, string> artwork = GetArtistArtwork(downloadedArtist);
          // check thumb stuff
          m_musicDatabase.SetArtForItem(downloadedArtist.idArtist, "artist", artwork);
          m_artistCache.insert(make_pair(*artistCredit, downloadedArtist));
        }
        else if (artistDownloadStatus == INFO_CANCELLED)
          break;
        else
        {
          // Cache the lookup failure so we don't retry
          artistTmp.idArtist = m_musicDatabase.AddArtist(artistCredit->GetArtist(), artistCredit->GetMusicBrainzArtistID());
          m_artistCache.insert(make_pair(*artistCredit, artistTmp));
        }
        
        // Update the cache pointer with our newly created info
        cachedArtist = m_artistCache.find(*artistCredit);
      }

      m_musicDatabase.AddAlbumArtist(cachedArtist->second.idArtist,
                                     cachedAlbum->second.idAlbum,
                                     artistCredit->GetJoinPhrase(),
                                     artistCredit == album->artistCredits.begin() ? false : true,
                                     std::distance(cachedAlbum->second.artistCredits.begin(), artistCredit));
    }

    if (m_bStop)
      break;

    for (VECSONGS::iterator song = album->songs.begin(); song != album->songs.end(); ++song)
    {
      song->idAlbum = cachedAlbum->second.idAlbum;
      song->idSong = m_musicDatabase.AddSong(cachedAlbum->second.idAlbum,
                                             song->strTitle, song->strMusicBrainzTrackID,
                                             song->strFileName, song->strComment,
                                             song->strThumb,
                                             song->artist, song->genre,
                                             song->iTrack, song->iDuration, song->iYear,
                                             song->iTimesPlayed, song->iStartOffset,
                                             song->iEndOffset,
                                             song->lastPlayed,
                                             song->rating,
                                             song->iKaraokeNumber);
      for (VECARTISTCREDITS::iterator artistCredit = song->artistCredits.begin(); artistCredit != song->artistCredits.end(); ++artistCredit)
      {
        if (m_bStop)
          break;

        // Check if the artist has already been downloaded or failed
        map<CArtistCredit, CArtist>::iterator cachedArtist = m_artistCache.find(*artistCredit);
        if (cachedArtist == m_artistCache.end())
        {
          CArtist artistTmp;
          artistTmp.strArtist = artistCredit->GetArtist();
          artistTmp.strMusicBrainzArtistID = artistCredit->GetMusicBrainzArtistID();
          URIUtils::GetParentPath(album->strPath, artistTmp.strPath); // FIXME

          // No - download the information
          CMusicArtistInfo artistInfo;
          INFO_RET artistDownloadStatus = INFO_NOT_FOUND;
          if (m_flags & SCAN_ONLINE)
            artistDownloadStatus = DownloadArtistInfo(artistTmp, artistScraper, artistInfo);

          if (artistDownloadStatus == INFO_ADDED || artistDownloadStatus == INFO_HAVE_ALREADY)
          {
            CArtist &downloadedArtist = artistInfo.GetArtist();
            downloadedArtist.idArtist = m_musicDatabase.AddArtist(downloadedArtist.strArtist,
                                                                  downloadedArtist.strMusicBrainzArtistID);
            m_musicDatabase.SetArtistInfo(downloadedArtist.idArtist,
                                          downloadedArtist);
            // check thumb stuff
            URIUtils::GetParentPath(album->strPath, downloadedArtist.strPath);
            map<string, string> artwork = GetArtistArtwork(downloadedArtist);
            m_musicDatabase.SetArtForItem(downloadedArtist.idArtist, "artist", artwork);
            m_artistCache.insert(make_pair(*artistCredit, downloadedArtist));
          }
          else if (artistDownloadStatus == INFO_CANCELLED)
            break;
          else
          {
            // Cache the lookup failure so we don't retry
            artistTmp.idArtist = m_musicDatabase.AddArtist(artistCredit->GetArtist(), artistCredit->GetMusicBrainzArtistID());
            m_artistCache.insert(make_pair(*artistCredit, artistTmp));
          }

          // Update the cache pointer with our newly created info
          cachedArtist = m_artistCache.find(*artistCredit);
        }

        m_musicDatabase.AddSongArtist(cachedArtist->second.idArtist,
                                      song->idSong,
                                      g_advancedSettings.m_musicItemSeparator, // we don't have song artist breakdowns from scrapers, yet
                                      artistCredit == song->artistCredits.begin() ? false : true,
                                      std::distance(song->artistCredits.begin(), artistCredit));
      }
    }

    if (m_bStop)
      break;

    // Commit the album to the DB
    m_musicDatabase.CommitTransaction();
    numAdded += album->songs.size();
  }

  if (m_bStop)
    m_musicDatabase.RollbackTransaction();

  if (m_handle)
    m_handle->SetTitle(g_localizeStrings.Get(505));

  return numAdded;
}

static bool SortSongsByTrack(const CSong& song, const CSong& song2)
{
  return song.iTrack < song2.iTrack;
}

void CMusicInfoScanner::FixupAlbums(VECALBUMS &albums)
{
  /*
   Step 2: Split into unique albums based on album name and album artist
   In the case where the album artist is unknown, we use the primary artist
   (i.e. first artist from each song).
   */
  for (VECALBUMS::iterator album = albums.begin(); album != albums.end(); ++album)
  {
    /*
     * If we have a valid MusicBrainz tag for the album, assume everything
     * is okay with our tags, as Picard should set everything up correctly.
     */
    if (!album->strMusicBrainzAlbumID.IsEmpty())
      continue;

    VECSONGS &songs = album->songs;
    // sort the songs by tracknumber to identify duplicate track numbers
    sort(songs.begin(), songs.end(), SortSongsByTrack);

    // map the songs to their primary artists
    bool tracksOverlap = false;
    bool hasAlbumArtist = false;
    bool isCompilation = true;

    map<string, vector<CSong *> > artists;
    for (VECSONGS::iterator song = songs.begin(); song != songs.end(); ++song)
    {
      // test for song overlap
      if (song != songs.begin() && song->iTrack == (song - 1)->iTrack)
        tracksOverlap = true;

      if (!song->bCompilation)
        isCompilation = false;

      // get primary artist
      string primary;
      if (!song->albumArtist.empty())
      {
        primary = song->albumArtist[0];
        hasAlbumArtist = true;
      }
      else if (!song->artist.empty())
        primary = song->artist[0];

      // add to the artist map
      artists[primary].push_back(&(*song));
    }

    /*
     We have a compilation if
     1. album name is non-empty AND
     2a. no tracks overlap OR
     2b. all tracks are marked as part of compilation AND
     3a. a unique primary artist is specified as "various" or "various artists" OR
     3b. we have at least two primary artists and no album artist specified.
     */
    bool compilation = !album->strAlbum.empty() && (isCompilation || !tracksOverlap); // 1+2b+2a
    if (artists.size() == 1)
    {
      string artist = artists.begin()->first; StringUtils::ToLower(artist);
      if (!StringUtils::EqualsNoCase(artist, "various") &&
          !StringUtils::EqualsNoCase(artist, "various artists")) // 3a
        compilation = false;
    }
    else if (hasAlbumArtist) // 3b
      compilation = false;

    if (compilation)
    {
      CLog::Log(LOGDEBUG, "Album '%s' is a compilation as there's no overlapping tracks and %s", album->strAlbum.c_str(), hasAlbumArtist ? "the album artist is 'Various'" : "there is more than one unique artist");
      artists.clear();
      std::string various = g_localizeStrings.Get(340); // Various Artists
      vector<string> va; va.push_back(various);
      for (VECSONGS::iterator song = songs.begin(); song != songs.end(); ++song)
      {
        song->albumArtist = va;
        artists[various].push_back(&(*song));
      }
    }

    /*
     Step 3: Find the common albumartist for each song and assign
     albumartist to those tracks that don't have it set.
     */
    for (map<string, vector<CSong *> >::iterator j = artists.begin(); j != artists.end(); ++j)
    {
      // find the common artist for these songs
      vector<CSong *> &artistSongs = j->second;
      vector<string> common = artistSongs.front()->albumArtist.empty() ? artistSongs.front()->artist : artistSongs.front()->albumArtist;
      for (vector<CSong *>::iterator k = artistSongs.begin() + 1; k != artistSongs.end(); ++k)
      {
        unsigned int match = 0;
        vector<string> &compare = (*k)->albumArtist.empty() ? (*k)->artist : (*k)->albumArtist;
        for (; match < common.size() && match < compare.size(); match++)
        {
          if (compare[match] != common[match])
            break;
        }
        common.erase(common.begin() + match, common.end());
      }

      /*
       Step 4: Assign the album artist for each song that doesn't have it set
       and add to the album vector
       */
      album->artistCredits.clear();
      for (vector<string>::iterator it = common.begin(); it != common.end(); ++it)
      {
        CStdString strJoinPhrase = (it == --common.end() ? "" : g_advancedSettings.m_musicItemSeparator);
        CArtistCredit artistCredit(*it, strJoinPhrase);
        album->artistCredits.push_back(artistCredit);
      }
      album->bCompilation = compilation;
      for (vector<CSong *>::iterator k = artistSongs.begin(); k != artistSongs.end(); ++k)
      {
        if ((*k)->albumArtist.empty())
          (*k)->albumArtist = common;
        // TODO: in future we may wish to union up the genres, for now we assume they're the same
        if (album->genre.empty())
          album->genre = (*k)->genre;
        //       in addition, we may want to use year as discriminating for albums
        if (album->iYear == 0)
          album->iYear = (*k)->iYear;
      }
    }
  }
}

void CMusicInfoScanner::FindArtForAlbums(VECALBUMS &albums, const CStdString &path)
{
  /*
   If there's a single album in the folder, then art can be taken from
   the folder art.
   */
  std::string albumArt;
  if (albums.size() == 1)
  {
    CFileItem album(path, true);
    albumArt = album.GetUserMusicThumb(true);
    if (!albumArt.empty())
      albums[0].art["thumb"] = albumArt;
  }
  for (VECALBUMS::iterator i = albums.begin(); i != albums.end(); ++i)
  {
    CAlbum &album = *i;

    if (albums.size() != 1)
      albumArt = "";

    /*
     Find art that is common across these items
     If we find a single art image we treat it as the album art
     and discard song art else we use first as album art and
     keep everything as song art.
     */
    bool singleArt = true;
    CSong *art = NULL;
    for (VECSONGS::iterator k = album.songs.begin(); k != album.songs.end(); ++k)
    {
      CSong &song = *k;
      if (song.HasArt())
      {
        if (art && !art->ArtMatches(song))
        {
          singleArt = false;
          break;
        }
        if (!art)
          art = &song;
      }
    }

    /*
      assign the first art found to the album - better than no art at all
    */

    if (art && albumArt.empty())
    {
      if (!art->strThumb.empty())
        albumArt = art->strThumb;
      else
        albumArt = CTextureCache::GetWrappedImageURL(art->strFileName, "music");
    }

    if (!albumArt.empty())
      album.art["thumb"] = albumArt;

    if (singleArt)
    { //if singleArt then we can clear the artwork for all songs
      for (VECSONGS::iterator k = album.songs.begin(); k != album.songs.end(); ++k)
        k->strThumb.clear();
    }
    else
    { // more than one piece of art was found for these songs, so cache per song
      for (VECSONGS::iterator k = album.songs.begin(); k != album.songs.end(); ++k)
      {
        if (k->strThumb.empty() && !k->embeddedArt.empty())
          k->strThumb = CTextureCache::GetWrappedImageURL(k->strFileName, "music");
      }
    }
  }
  if (albums.size() == 1 && !albumArt.empty())
  { // assign to folder thumb as well
    CMusicThumbLoader::SetCachedImage(path, "thumb", albumArt);
  }
}

int CMusicInfoScanner::GetPathHash(const CFileItemList &items, CStdString &hash)
{
  // Create a hash based on the filenames, filesize and filedate.  Also count the number of files
  if (0 == items.Size()) return 0;
  XBMC::XBMC_MD5 md5state;
  int count = 0;
  for (int i = 0; i < items.Size(); ++i)
  {
    const CFileItemPtr pItem = items[i];
    md5state.append(pItem->GetPath());
    md5state.append((unsigned char *)&pItem->m_dwSize, sizeof(pItem->m_dwSize));
    FILETIME time = pItem->m_dateTime;
    md5state.append((unsigned char *)&time, sizeof(FILETIME));
    if (pItem->IsAudio() && !pItem->IsPlayList() && !pItem->IsNFO())
      count++;
  }
  md5state.getDigest(hash);
  return count;
}

INFO_RET CMusicInfoScanner::UpdateDatabaseAlbumInfo(const CStdString& strPath, CMusicAlbumInfo& albumInfo, bool bAllowSelection, CGUIDialogProgress* pDialog /* = NULL */)
{
  m_musicDatabase.Open();
  CQueryParams params;
  CDirectoryNode::GetDatabaseInfo(strPath, params);

  if (params.GetAlbumId() == -1)
    return INFO_ERROR;

  CAlbum album;
  m_musicDatabase.GetAlbumInfo(params.GetAlbumId(), album, &album.songs);

  // find album info
  ADDON::ScraperPtr scraper;
  if (!m_musicDatabase.GetScraperForPath(strPath, scraper, ADDON::ADDON_SCRAPER_ALBUMS) || !scraper)
    return INFO_ERROR;
  m_musicDatabase.Close();

loop:
  CLog::Log(LOGDEBUG, "%s downloading info for: %s", __FUNCTION__, album.strAlbum.c_str());
  INFO_RET albumDownloadStatus = DownloadAlbumInfo(album, scraper, albumInfo, pDialog);
  if (albumDownloadStatus == INFO_NOT_FOUND)
  {
    if (pDialog && bAllowSelection)
    {
      if (!CGUIKeyboardFactory::ShowAndGetInput(album.strAlbum, g_localizeStrings.Get(16011), false))
        return INFO_CANCELLED;

      CStdString strTempArtist(StringUtils::Join(album.artist, g_advancedSettings.m_musicItemSeparator));
      if (!CGUIKeyboardFactory::ShowAndGetInput(strTempArtist, g_localizeStrings.Get(16025), false))
        return INFO_CANCELLED;

      album.artist = StringUtils::Split(strTempArtist, g_advancedSettings.m_musicItemSeparator);
      goto loop;
    }
  }
  else if (albumDownloadStatus == INFO_ADDED)
  {
    m_musicDatabase.Open();
    m_musicDatabase.SetAlbumInfo(params.GetAlbumId(), albumInfo.GetAlbum(), albumInfo.GetAlbum().songs);
    GetAlbumArtwork(params.GetAlbumId(), albumInfo.GetAlbum());
    albumInfo.SetLoaded(true);
    m_musicDatabase.Close();
  }
  return albumDownloadStatus;
}

INFO_RET CMusicInfoScanner::UpdateDatabaseArtistInfo(const CStdString& strPath, CMusicArtistInfo& artistInfo, bool bAllowSelection, CGUIDialogProgress* pDialog /* = NULL */)
{
  m_musicDatabase.Open();
  CQueryParams params;
  CDirectoryNode::GetDatabaseInfo(strPath, params);

  if (params.GetArtistId() == -1)
    return INFO_ERROR;

  CArtist artist;
  m_musicDatabase.GetArtistInfo(params.GetArtistId(), artist);

  // find album info
  ADDON::ScraperPtr scraper;
  if (!m_musicDatabase.GetScraperForPath(strPath, scraper, ADDON::ADDON_SCRAPER_ARTISTS) || !scraper)
    return INFO_ERROR;
  m_musicDatabase.Close();

loop:
  CLog::Log(LOGDEBUG, "%s downloading info for: %s", __FUNCTION__, artist.strArtist.c_str());
  INFO_RET artistDownloadStatus = DownloadArtistInfo(artist, scraper, artistInfo, pDialog);
  if (artistDownloadStatus == INFO_NOT_FOUND)
  {
    if (pDialog && bAllowSelection)
    {
      if (!CGUIKeyboardFactory::ShowAndGetInput(artist.strArtist, g_localizeStrings.Get(16025), false))
        return INFO_CANCELLED;
      goto loop;
    }
  }
  else if (artistDownloadStatus == INFO_ADDED)
  {
    m_musicDatabase.Open();
    m_musicDatabase.SetArtistInfo(params.GetArtistId(), artistInfo.GetArtist());
    m_musicDatabase.GetArtistPath(params.GetArtistId(), artist.strPath);
    map<string, string> artwork = GetArtistArtwork(artist);
    m_musicDatabase.SetArtForItem(params.GetArtistId(), "artist", artwork);
    artistInfo.SetLoaded();
    m_musicDatabase.Close();
  }
  return artistDownloadStatus;
}

#define THRESHOLD .95f

INFO_RET CMusicInfoScanner::DownloadAlbumInfo(const CAlbum& album, ADDON::ScraperPtr& info, CMusicAlbumInfo& albumInfo, CGUIDialogProgress* pDialog)
{
  if (m_handle)
  {
    m_handle->SetTitle(StringUtils::Format(g_localizeStrings.Get(20321), info->Name().c_str()));
    m_handle->SetText(StringUtils::Join(album.artist, g_advancedSettings.m_musicItemSeparator) + " - " + album.strAlbum);
  }

  // clear our scraper cache
  info->ClearCache();

  CMusicInfoScraper scraper(info);
  bool bMusicBrainz = false;
  if (!album.strMusicBrainzAlbumID.empty())
  {
    CScraperUrl musicBrainzURL;
    if (ResolveMusicBrainz(album.strMusicBrainzAlbumID, info, scraper, musicBrainzURL))
    {
      CMusicAlbumInfo albumNfo("nfo", musicBrainzURL);
      scraper.GetAlbums().clear();
      scraper.GetAlbums().push_back(albumNfo);
      bMusicBrainz = true;
    }
  }

  // handle nfo files
  CStdString strNfo;
  URIUtils::AddFileToFolder(album.strPath, "album.nfo", strNfo);
  CNfoFile::NFOResult result = CNfoFile::NO_NFO;
  CNfoFile nfoReader;
  if (XFILE::CFile::Exists(strNfo))
  {
    CLog::Log(LOGDEBUG,"Found matching nfo file: %s", strNfo.c_str());
    result = nfoReader.Create(strNfo, info, -1, album.strPath);
    if (result == CNfoFile::FULL_NFO)
    {
      CLog::Log(LOGDEBUG, "%s Got details from nfo", __FUNCTION__);
      nfoReader.GetDetails(albumInfo.GetAlbum());
      return INFO_ADDED;
    }
    else if (result == CNfoFile::URL_NFO || result == CNfoFile::COMBINED_NFO)
    {
      CScraperUrl scrUrl(nfoReader.ScraperUrl());
      CMusicAlbumInfo albumNfo("nfo",scrUrl);
      info = nfoReader.GetScraperInfo();
      CLog::Log(LOGDEBUG,"-- nfo-scraper: %s",info->Name().c_str());
      CLog::Log(LOGDEBUG,"-- nfo url: %s", scrUrl.m_url[0].m_url.c_str());
      scraper.SetScraperInfo(info);
      scraper.GetAlbums().clear();
      scraper.GetAlbums().push_back(albumNfo);
    }
    else
      CLog::Log(LOGERROR,"Unable to find an url in nfo file: %s", strNfo.c_str());
  }

  if (!scraper.CheckValidOrFallback(CSettings::Get().GetString("musiclibrary.albumsscraper")))
  { // the current scraper is invalid, as is the default - bail
    CLog::Log(LOGERROR, "%s - current and default scrapers are invalid.  Pick another one", __FUNCTION__);
    return INFO_ERROR;
  }

  if (!scraper.GetAlbumCount())
  {
    scraper.FindAlbumInfo(album.strAlbum, StringUtils::Join(album.artist, g_advancedSettings.m_musicItemSeparator));

    while (!scraper.Completed())
    {
      if (m_bStop)
      {
        scraper.Cancel();
        return INFO_CANCELLED;
      }
      Sleep(1);
    }
  }

  CGUIDialogSelect *pDlg = NULL;
  int iSelectedAlbum=0;
  if (result == CNfoFile::NO_NFO && !bMusicBrainz)
  {
    iSelectedAlbum = -1; // set negative so that we can detect a failure
    if (scraper.Succeeded() && scraper.GetAlbumCount() >= 1)
    {
      double bestRelevance = 0;
      double minRelevance = THRESHOLD;
      if (scraper.GetAlbumCount() > 1) // score the matches
      {
        //show dialog with all albums found
        if (pDialog)
        {
          pDlg = (CGUIDialogSelect*)g_windowManager.GetWindow(WINDOW_DIALOG_SELECT);
          pDlg->SetHeading(g_localizeStrings.Get(181).c_str());
          pDlg->Reset();
          pDlg->EnableButton(true, 413); // manual
        }

        for (int i = 0; i < scraper.GetAlbumCount(); ++i)
        {
          CMusicAlbumInfo& info = scraper.GetAlbum(i);
          double relevance = info.GetRelevance();
          if (relevance < 0)
            relevance = CUtil::AlbumRelevance(info.GetAlbum().strAlbum, album.strAlbum, StringUtils::Join(info.GetAlbum().artist, g_advancedSettings.m_musicItemSeparator), StringUtils::Join(album.artist, g_advancedSettings.m_musicItemSeparator));

          // if we're doing auto-selection (ie querying all albums at once, then allow 95->100% for perfect matches)
          // otherwise, perfect matches only
          if (relevance >= max(minRelevance, bestRelevance))
          { // we auto-select the best of these
            bestRelevance = relevance;
            iSelectedAlbum = i;
          }
          if (pDialog)
          {
            // set the label to [relevance]  album - artist
            CStdString strTemp;
            strTemp.Format("[%0.2f]  %s", relevance, info.GetTitle2());
            CFileItem item(strTemp);
            item.m_idepth = i; // use this to hold the index of the album in the scraper
            pDlg->Add(&item);
          }
          if (relevance > .99f) // we're so close, no reason to search further
            break;
        }

        if (pDialog && bestRelevance < THRESHOLD)
        {
          pDlg->Sort(false);
          pDlg->DoModal();

          // and wait till user selects one
          if (pDlg->GetSelectedLabel() < 0)
          { // none chosen
            if (!pDlg->IsButtonPressed())
              return INFO_CANCELLED;

            // manual button pressed
            CStdString strNewAlbum = album.strAlbum;
            if (!CGUIKeyboardFactory::ShowAndGetInput(strNewAlbum, g_localizeStrings.Get(16011), false)) return INFO_CANCELLED;
            if (strNewAlbum == "") return INFO_CANCELLED;

            CStdString strNewArtist = StringUtils::Join(album.artist, g_advancedSettings.m_musicItemSeparator);
            if (!CGUIKeyboardFactory::ShowAndGetInput(strNewArtist, g_localizeStrings.Get(16025), false)) return INFO_CANCELLED;

            pDialog->SetLine(0, strNewAlbum);
            pDialog->SetLine(1, strNewArtist);
            pDialog->Progress();

            CAlbum newAlbum = album;
            newAlbum.strAlbum = strNewAlbum;
            newAlbum.artist = StringUtils::Split(strNewArtist, g_advancedSettings.m_musicItemSeparator);

            return DownloadAlbumInfo(newAlbum, info, albumInfo, pDialog);
          }
          iSelectedAlbum = pDlg->GetSelectedItem()->m_idepth;
        }
      }
      else
      {
        CMusicAlbumInfo& info = scraper.GetAlbum(0);
        double relevance = info.GetRelevance();
        if (relevance < 0)
          relevance = CUtil::AlbumRelevance(info.GetAlbum().strAlbum,
                                            album.strAlbum,
                                            StringUtils::Join(info.GetAlbum().artist, g_advancedSettings.m_musicItemSeparator),
                                            StringUtils::Join(album.artist, g_advancedSettings.m_musicItemSeparator));
        if (relevance < THRESHOLD)
          return INFO_NOT_FOUND;

        iSelectedAlbum = 0;
      }
    }

    if (iSelectedAlbum < 0)
      return INFO_NOT_FOUND;

  }

  scraper.LoadAlbumInfo(iSelectedAlbum);
  while (!scraper.Completed())
  {
    if (m_bStop)
    {
      scraper.Cancel();
      return INFO_CANCELLED;
    }
    Sleep(1);
  }

  if (!scraper.Succeeded())
    return INFO_ERROR;

  albumInfo = scraper.GetAlbum(iSelectedAlbum);
  
  if (result == CNfoFile::COMBINED_NFO)
    nfoReader.GetDetails(albumInfo.GetAlbum(), NULL, true);
  
  return INFO_ADDED;
}

void CMusicInfoScanner::GetAlbumArtwork(long id, const CAlbum &album)
{
  if (album.thumbURL.m_url.size())
  {
    if (m_musicDatabase.GetArtForItem(id, "album", "thumb").empty())
    {
      string thumb = CScraperUrl::GetThumbURL(album.thumbURL.GetFirstThumb());
      if (!thumb.empty())
      {
        CTextureCache::Get().BackgroundCacheImage(thumb);
        m_musicDatabase.SetArtForItem(id, "album", "thumb", thumb);
      }
    }
  }
}

INFO_RET CMusicInfoScanner::DownloadArtistInfo(const CArtist& artist, ADDON::ScraperPtr& info, MUSIC_GRABBER::CMusicArtistInfo& artistInfo, CGUIDialogProgress* pDialog)
{
  if (m_handle)
  {
    m_handle->SetTitle(StringUtils::Format(g_localizeStrings.Get(20320), info->Name().c_str()));
    m_handle->SetText(artist.strArtist);
  }

  // clear our scraper cache
  info->ClearCache();

  CMusicInfoScraper scraper(info);
  bool bMusicBrainz = false;
  if (!artist.strMusicBrainzArtistID.empty())
  {
    CScraperUrl musicBrainzURL;
    if (ResolveMusicBrainz(artist.strMusicBrainzArtistID, info, scraper, musicBrainzURL))
    {
      CMusicArtistInfo artistNfo("nfo", musicBrainzURL);
      scraper.GetArtists().clear();
      scraper.GetArtists().push_back(artistNfo);
      bMusicBrainz = true;
    }
  }

  // handle nfo files
  CStdString strNfo;
  URIUtils::AddFileToFolder(artist.strPath, "artist.nfo", strNfo);
  CNfoFile::NFOResult result=CNfoFile::NO_NFO;
  CNfoFile nfoReader;
  if (XFILE::CFile::Exists(strNfo))
  {
    CLog::Log(LOGDEBUG,"Found matching nfo file: %s", strNfo.c_str());
    result = nfoReader.Create(strNfo, info);
    if (result == CNfoFile::FULL_NFO)
    {
      CLog::Log(LOGDEBUG, "%s Got details from nfo", __FUNCTION__);
      nfoReader.GetDetails(artistInfo.GetArtist());
      return INFO_ADDED;
    }
    else if (result == CNfoFile::URL_NFO || result == CNfoFile::COMBINED_NFO)
    {
      CScraperUrl scrUrl(nfoReader.ScraperUrl());
      CMusicArtistInfo artistNfo("nfo",scrUrl);
      info = nfoReader.GetScraperInfo();
      CLog::Log(LOGDEBUG,"-- nfo-scraper: %s",info->Name().c_str());
      CLog::Log(LOGDEBUG,"-- nfo url: %s", scrUrl.m_url[0].m_url.c_str());
      scraper.SetScraperInfo(info);
      scraper.GetArtists().push_back(artistNfo);
    }
    else
      CLog::Log(LOGERROR,"Unable to find an url in nfo file: %s", strNfo.c_str());
  }

  if (!scraper.GetArtistCount())
  {
    scraper.FindArtistInfo(artist.strArtist);

    while (!scraper.Completed())
    {
      if (m_bStop)
      {
        scraper.Cancel();
        return INFO_CANCELLED;
      }
      Sleep(1);
    }
  }

  int iSelectedArtist = 0;
  if (result == CNfoFile::NO_NFO && !bMusicBrainz)
  {
    if (scraper.GetArtistCount() >= 1)
    {
      // now load the first match
      if (pDialog && scraper.GetArtistCount() > 1)
      {
        // if we found more then 1 album, let user choose one
        CGUIDialogSelect *pDlg = (CGUIDialogSelect*)g_windowManager.GetWindow(WINDOW_DIALOG_SELECT);
        if (pDlg)
        {
          pDlg->SetHeading(g_localizeStrings.Get(21890));
          pDlg->Reset();
          pDlg->EnableButton(true, 413); // manual

          for (int i = 0; i < scraper.GetArtistCount(); ++i)
          {
            // set the label to artist
            CFileItem item(scraper.GetArtist(i).GetArtist());
            CStdString strTemp=scraper.GetArtist(i).GetArtist().strArtist;
            if (!scraper.GetArtist(i).GetArtist().strBorn.IsEmpty())
              strTemp += " ("+scraper.GetArtist(i).GetArtist().strBorn+")";
            if (!scraper.GetArtist(i).GetArtist().genre.empty())
            {
              CStdString genres = StringUtils::Join(scraper.GetArtist(i).GetArtist().genre, g_advancedSettings.m_musicItemSeparator);
              if (!genres.empty())
                strTemp.Format("[%s] %s", genres.c_str(), strTemp.c_str());
            }
            item.SetLabel(strTemp);
            item.m_idepth = i; // use this to hold the index of the album in the scraper
            pDlg->Add(&item);
          }
          pDlg->DoModal();

          // and wait till user selects one
          if (pDlg->GetSelectedLabel() < 0)
          { // none chosen
            if (!pDlg->IsButtonPressed())
              return INFO_CANCELLED;

            // manual button pressed
            CStdString strNewArtist = artist.strArtist;
            if (!CGUIKeyboardFactory::ShowAndGetInput(strNewArtist, g_localizeStrings.Get(16025), false)) return INFO_CANCELLED;

            if (pDialog)
            {
              pDialog->SetLine(0, strNewArtist);
              pDialog->Progress();
            }

            CArtist newArtist;
            newArtist.strArtist = strNewArtist;
            return DownloadArtistInfo(newArtist, info, artistInfo, pDialog);
          }
          iSelectedArtist = pDlg->GetSelectedItem()->m_idepth;
        }
      }
    }
    else
      return INFO_NOT_FOUND;
  }

  scraper.LoadArtistInfo(iSelectedArtist, artist.strArtist);
  while (!scraper.Completed())
  {
    if (m_bStop)
    {
      scraper.Cancel();
      return INFO_CANCELLED;
    }
    Sleep(1);
  }

  if (!scraper.Succeeded())
    return INFO_ERROR;

  artistInfo = scraper.GetArtist(iSelectedArtist);

  if (result == CNfoFile::COMBINED_NFO)
    nfoReader.GetDetails(artistInfo.GetArtist(), NULL, true);

  return INFO_ADDED;
}

bool CMusicInfoScanner::ResolveMusicBrainz(const CStdString strMusicBrainzID, ScraperPtr &preferredScraper, CMusicInfoScraper &musicInfoScraper, CScraperUrl &musicBrainzURL)
{
  // We have a MusicBrainz ID
  // Get a scraper that can resolve it to a MusicBrainz URL & force our
  // search directly to the specific album.
  bool bMusicBrainz = false;
  ADDON::TYPE type = ScraperTypeFromContent(preferredScraper->Content());

  CFileItemList items;
  ADDON::AddonPtr addon;
  ADDON::ScraperPtr defaultScraper;
  if (ADDON::CAddonMgr::Get().GetDefault(type, addon))
    defaultScraper = boost::dynamic_pointer_cast<CScraper>(addon);

  vector<ScraperPtr> vecScrapers;

  // add selected scraper - first proirity
  if (preferredScraper)
    vecScrapers.push_back(preferredScraper);

  // Add all scrapers except selected and default
  VECADDONS addons;
  CAddonMgr::Get().GetAddons(type, addons);

  for (unsigned i = 0; i < addons.size(); ++i)
  {
    ScraperPtr scraper = boost::dynamic_pointer_cast<CScraper>(addons[i]);

    // skip if scraper requires settings and there's nothing set yet
    if (!scraper || (scraper->RequiresSettings() && !scraper->HasUserSettings()))
      continue;
    
    if((!preferredScraper || preferredScraper->ID() != scraper->ID()) && (!defaultScraper || defaultScraper->ID() != scraper->ID()) )
      vecScrapers.push_back(scraper);
  }

  // add default scraper - not user selectable so it's last priority
  if(defaultScraper && 
     (!preferredScraper || preferredScraper->ID() != defaultScraper->ID()) &&
     (!defaultScraper->RequiresSettings() || defaultScraper->HasUserSettings()))
    vecScrapers.push_back(defaultScraper);

  for (unsigned int i=0; i < vecScrapers.size(); ++i)
  {
    if (vecScrapers[i]->Type() != type)
      continue;

    vecScrapers[i]->ClearCache();
    try
    {
      musicBrainzURL = vecScrapers[i]->ResolveIDToUrl(strMusicBrainzID);
    }
    catch (const ADDON::CScraperError &sce)
    {
      if (!sce.FAborted())
        continue;
    }
    if (!musicBrainzURL.m_url.empty())
    {
      CLog::Log(LOGDEBUG,"-- nfo-scraper: %s",vecScrapers[i]->Name().c_str());
      CLog::Log(LOGDEBUG,"-- nfo url: %s", musicBrainzURL.m_url[0].m_url.c_str());
      musicInfoScraper.SetScraperInfo(vecScrapers[i]);
      bMusicBrainz = true;
      break;
    }
  }

  return bMusicBrainz;
}

map<string, string> CMusicInfoScanner::GetArtistArtwork(const CArtist& artist)
{
  map<string, string> artwork;

  // check thumb
  CStdString strFolder;
  CStdString thumb;
  if (!artist.strPath.IsEmpty())
  {
    strFolder = artist.strPath;
    for (int i = 0; i < 3 && thumb.IsEmpty(); ++i)
    {
      CFileItem item(strFolder, true);
      thumb = item.GetUserMusicThumb(true);
      strFolder = URIUtils::GetParentPath(strFolder);
    }
  }
  if (thumb.IsEmpty())
    thumb = CScraperUrl::GetThumbURL(artist.thumbURL.GetFirstThumb());
  if (!thumb.IsEmpty())
  {
    CTextureCache::Get().BackgroundCacheImage(thumb);
    artwork.insert(make_pair("thumb", thumb));
  }

  // check fanart
  CStdString fanart;
  if (!artist.strPath.IsEmpty())
  {
    strFolder = artist.strPath;
    for (int i = 0; i < 3 && fanart.IsEmpty(); ++i)
    {
      CFileItem item(strFolder, true);
      fanart = item.GetLocalFanart();
      strFolder = URIUtils::GetParentPath(strFolder);
    }
  }
  if (fanart.IsEmpty())
    fanart = artist.fanart.GetImageURL();
  if (!fanart.IsEmpty())
  {
    CTextureCache::Get().BackgroundCacheImage(fanart);
    artwork.insert(make_pair("fanart", fanart));
  }

  return artwork;
}

// This function is the Run() function of the IRunnable
// CFileCountReader and runs in a separate thread.
void CMusicInfoScanner::Run()
{
  int count = 0;
  for (set<std::string>::iterator it = m_pathsToScan.begin(); it != m_pathsToScan.end() && !m_bStop; ++it)
  {
    count+=CountFilesRecursively(*it);
  }
  m_itemCount = count;
}

// Recurse through all folders we scan and count files
int CMusicInfoScanner::CountFilesRecursively(const CStdString& strPath)
{
  // load subfolder
  CFileItemList items;
  CDirectory::GetDirectory(strPath, items, g_advancedSettings.m_musicExtensions, DIR_FLAG_NO_FILE_DIRS);

  if (m_bStop)
    return 0;

  // true for recursive counting
  int count = CountFiles(items, true);
  return count;
}

int CMusicInfoScanner::CountFiles(const CFileItemList &items, bool recursive)
{
  int count = 0;
  for (int i=0; i<items.Size(); ++i)
  {
    const CFileItemPtr pItem=items[i];
    
    if (recursive && pItem->m_bIsFolder)
      count+=CountFilesRecursively(pItem->GetPath());
    else if (pItem->IsAudio() && !pItem->IsPlayList() && !pItem->IsNFO())
      count++;
  }
  return count;
}
