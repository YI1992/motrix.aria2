/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2026 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "Sqlite3BtProgressInfoFile.h"

#ifdef HAVE_SQLITE3

#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <thread>

#include <cppunit/extensions/HelperMacros.h>

#include "BitfieldMan.h"
#include "DownloadContext.h"
#include "FileEntry.h"
#include "GroupId.h"
#include "MockPieceStorage.h"
#include "Option.h"
#include "RequestGroup.h"
#include "Sqlite3PersistenceStore.h"
#include "TestUtil.h"
#include "array_fun.h"
#include "prefs.h"
#include "util.h"

#ifdef ENABLE_BITTORRENT
#  include "BtRuntime.h"
#  include "MockPeerStorage.h"
#  include "bittorrent_helper.h"
#endif

namespace aria2 {

class Sqlite3BtProgressInfoFileTest : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(Sqlite3BtProgressInfoFileTest);
#ifdef ENABLE_BITTORRENT
  CPPUNIT_TEST(testSaveRoundTrip);
  CPPUNIT_TEST(testDirtySkipUnchangedContent);
  CPPUNIT_TEST(testCheckpointFoundByPathUnderNewGid);
  CPPUNIT_TEST(testCheckpointSurvivesTaskRowDeletion);
  CPPUNIT_TEST(testSaveUnderNewGidKeepsOneRowPerPath);
  CPPUNIT_TEST(testRemoveFileDropsCheckpointByPath);
  CPPUNIT_TEST(testLegacyGidRowIsFoundAndReplaced);
  CPPUNIT_TEST(testPruneDropsOnlyDefunctOrphans);
#endif
  CPPUNIT_TEST_SUITE_END();

private:
  std::shared_ptr<Option> option_;
  std::shared_ptr<DownloadContext> dctx_;
  std::shared_ptr<MockPieceStorage> pieceStorage_;
  std::shared_ptr<BitfieldMan> bitfield_;
  std::shared_ptr<RequestGroup> rg_;
  std::unique_ptr<Sqlite3PersistenceStore> store_;
  std::string dbPath_;
#ifdef ENABLE_BITTORRENT
  std::shared_ptr<MockPeerStorage> peerStorage_;
  std::shared_ptr<BtRuntime> btRuntime_;
#endif

public:
  void setUp() override
  {
    option_.reset(new Option());
    option_->put(PREF_DIR, A2_TEST_OUT_DIR);

    dbPath_ = std::string(A2_TEST_OUT_DIR) + "/sqlite3-bt-progress-test.db";
    std::remove(dbPath_.c_str());
    std::remove((dbPath_ + "-wal").c_str());
    std::remove((dbPath_ + "-shm").c_str());

    store_.reset(new Sqlite3PersistenceStore(dbPath_));
    store_->open();

    bitfield_.reset(new BitfieldMan(1_k, 80_k));
    pieceStorage_.reset(new MockPieceStorage());
    pieceStorage_->setBitfield(bitfield_.get());

#ifdef ENABLE_BITTORRENT
    static unsigned char infoHash[] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa,
        0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff};
    dctx_.reset(new DownloadContext());
    {
      auto torrentAttrs = make_unique<TorrentAttribute>();
      torrentAttrs->infoHash.assign(std::begin(infoHash), std::end(infoHash));
      dctx_->setAttribute(CTX_ATTR_BT, std::move(torrentAttrs));
    }
    const std::shared_ptr<FileEntry> fileEntries[] = {
        std::shared_ptr<FileEntry>(new FileEntry("/path/to/file", 80_k, 0))};
    dctx_->setFileEntries(std::begin(fileEntries), std::end(fileEntries));
    dctx_->setPieceLength(1_k);
    peerStorage_.reset(new MockPeerStorage());
    btRuntime_.reset(new BtRuntime());
#endif

    // gid required so Sqlite3BtProgressInfoFile can derive gidHex_.
    auto gid = GroupId::import(0xdeadbeefcafebabeULL);
    rg_.reset(new RequestGroup(gid, option_));
    rg_->setDownloadContext(dctx_);
    dctx_->setOwnerRequestGroup(rg_.get());

    // task_progress has FOREIGN KEY (gid) REFERENCES task(gid) ON DELETE
    // CASCADE. Insert a parent task row so the foreign-key insert succeeds.
    sqlite3* db = store_->raw();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(
        db,
        "INSERT INTO task(gid, state, serialized, queue_position, digest,"
        " created_at, updated_at)"
        " VALUES (?, 'waiting', '', 0, X'', 0, 0)",
        -1, &stmt, nullptr);
    auto gidHex = GroupId::toHex(rg_->getGID());
    sqlite3_bind_text(stmt, 1, gidHex.data(), gidHex.size(), SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void tearDown() override
  {
    rg_.reset();
    if (store_) {
      store_->finalCheckpointAndClose();
    }
    store_.reset();
    std::remove(dbPath_.c_str());
    std::remove((dbPath_ + "-wal").c_str());
    std::remove((dbPath_ + "-shm").c_str());
  }

#ifdef ENABLE_BITTORRENT
  void testSaveRoundTrip();
  void testDirtySkipUnchangedContent();
  void testCheckpointFoundByPathUnderNewGid();
  void testCheckpointSurvivesTaskRowDeletion();
  void testSaveUnderNewGidKeepsOneRowPerPath();
  void testRemoveFileDropsCheckpointByPath();
  void testLegacyGidRowIsFoundAndReplaced();
  void testPruneDropsOnlyDefunctOrphans();

private:
  // A second download of the same output path under a different gid — the
  // shape of a front-end retry that re-adds a task instead of unpausing it.
  struct Rebound {
    std::shared_ptr<RequestGroup> rg;
    std::shared_ptr<DownloadContext> dctx;
  };
  Rebound rebind(a2_gid_t gid, const std::string& path);
  int64_t count(const std::string& sql, const std::string& arg);
  void exec(const std::string& sql);
#endif
};

CPPUNIT_TEST_SUITE_REGISTRATION(Sqlite3BtProgressInfoFileTest);

#ifdef ENABLE_BITTORRENT
void Sqlite3BtProgressInfoFileTest::testSaveRoundTrip()
{
  bitfield_->setBit(0);
  bitfield_->setBit(3);
  bitfield_->setBit(70);

  Sqlite3BtProgressInfoFile saver(dctx_, pieceStorage_, option_.get(),
                                  store_.get());
  saver.setBtRuntime(btRuntime_);
  saver.setPeerStorage(peerStorage_);
  saver.save();

  // Fresh BitfieldMan + PieceStorage on the load side to avoid trivial pass.
  auto loadBf = std::make_shared<BitfieldMan>(1_k, 80_k);
  auto loadPs = std::make_shared<MockPieceStorage>();
  loadPs->setBitfield(loadBf.get());

  Sqlite3BtProgressInfoFile loader(dctx_, loadPs, option_.get(), store_.get());
  loader.setBtRuntime(btRuntime_);
  loader.setPeerStorage(peerStorage_);
  CPPUNIT_ASSERT(loader.exists());
  loader.load();

  CPPUNIT_ASSERT(loadBf->isBitSet(0));
  CPPUNIT_ASSERT(loadBf->isBitSet(3));
  CPPUNIT_ASSERT(loadBf->isBitSet(70));
  CPPUNIT_ASSERT(!loadBf->isBitSet(1));
  CPPUNIT_ASSERT(!loadBf->isBitSet(50));
}

void Sqlite3BtProgressInfoFileTest::testDirtySkipUnchangedContent()
{
  bitfield_->setBit(0);
  bitfield_->setBit(3);
  bitfield_->setBit(70);

  Sqlite3BtProgressInfoFile saver(dctx_, pieceStorage_, option_.get(),
                                  store_.get());
  saver.setBtRuntime(btRuntime_);
  saver.setPeerStorage(peerStorage_);

  saver.save();

  // Query the row's updated_at right after the first save.
  auto queryUpdatedAt = [&]() -> int64_t {
    sqlite3_stmt* stmt = nullptr;
    CPPUNIT_ASSERT_EQUAL(
        SQLITE_OK,
        sqlite3_prepare_v2(store_->raw(),
                           "SELECT updated_at FROM task_progress WHERE gid = ?",
                           -1, &stmt, nullptr));
    auto gidHex = GroupId::toHex(rg_->getGID());
    sqlite3_bind_text(stmt, 1, gidHex.data(), gidHex.size(), SQLITE_STATIC);
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    int64_t v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
  };

  int64_t firstUpdatedAt = queryUpdatedAt();

  // Sleep so wall-clock advances enough that a redundant UPSERT would change
  // updated_at to a different millisecond.
  std::this_thread::sleep_for(std::chrono::milliseconds(2));

  // Save again with no in-memory state mutation between calls — dirty-skip
  // path must short-circuit before any UPSERT runs.
  saver.save();

  int64_t secondUpdatedAt = queryUpdatedAt();

  CPPUNIT_ASSERT_EQUAL(firstUpdatedAt, secondUpdatedAt);
}

Sqlite3BtProgressInfoFileTest::Rebound
Sqlite3BtProgressInfoFileTest::rebind(a2_gid_t gid, const std::string& path)
{
  Rebound r;
  r.dctx = std::make_shared<DownloadContext>();
  {
    auto torrentAttrs = make_unique<TorrentAttribute>();
    const unsigned char* hash = bittorrent::getInfoHash(dctx_);
    torrentAttrs->infoHash.assign(hash, hash + INFO_HASH_LENGTH);
    r.dctx->setAttribute(CTX_ATTR_BT, std::move(torrentAttrs));
  }
  const std::shared_ptr<FileEntry> entries[] = {
      std::make_shared<FileEntry>(path, 80_k, 0)};
  r.dctx->setFileEntries(std::begin(entries), std::end(entries));
  r.dctx->setPieceLength(1_k);
  r.rg = std::make_shared<RequestGroup>(GroupId::import(gid), option_);
  r.rg->setDownloadContext(r.dctx);
  r.dctx->setOwnerRequestGroup(r.rg.get());
  return r;
}

int64_t Sqlite3BtProgressInfoFileTest::count(const std::string& sql,
                                             const std::string& arg)
{
  sqlite3_stmt* stmt = nullptr;
  CPPUNIT_ASSERT_EQUAL(SQLITE_OK, sqlite3_prepare_v2(store_->raw(), sql.c_str(),
                                                     -1, &stmt, nullptr));
  sqlite3_bind_text(stmt, 1, arg.data(), arg.size(), SQLITE_TRANSIENT);
  CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
  int64_t n = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

void Sqlite3BtProgressInfoFileTest::exec(const std::string& sql)
{
  char* err = nullptr;
  int rc = sqlite3_exec(store_->raw(), sql.c_str(), nullptr, nullptr, &err);
  std::string msg = err ? err : "";
  sqlite3_free(err);
  CPPUNIT_ASSERT_EQUAL_MESSAGE(msg, SQLITE_OK, rc);
}

void Sqlite3BtProgressInfoFileTest::testCheckpointFoundByPathUnderNewGid()
{
  // A retry re-adds the download under a fresh gid. Like a `.aria2` control
  // file, the checkpoint must be addressed by the output path so the new gid
  // still resumes from it (Motrix#2187).
  bitfield_->setBit(5);
  Sqlite3BtProgressInfoFile saver(dctx_, pieceStorage_, option_.get(),
                                  store_.get());
  saver.setBtRuntime(btRuntime_);
  saver.setPeerStorage(peerStorage_);
  saver.save();

  auto next = rebind(0x1111222233334444ULL, "/path/to/file");
  auto loadBf = std::make_shared<BitfieldMan>(1_k, 80_k);
  auto loadPs = std::make_shared<MockPieceStorage>();
  loadPs->setBitfield(loadBf.get());
  Sqlite3BtProgressInfoFile loader(next.dctx, loadPs, option_.get(),
                                   store_.get());
  loader.setBtRuntime(btRuntime_);
  loader.setPeerStorage(peerStorage_);
  CPPUNIT_ASSERT(loader.exists());
  loader.load();
  CPPUNIT_ASSERT(loadBf->isBitSet(5));

  // A different path under the same kind of retry finds nothing.
  auto other = rebind(0x5555666677778888ULL, "/path/to/other");
  Sqlite3BtProgressInfoFile stranger(other.dctx, loadPs, option_.get(),
                                     store_.get());
  CPPUNIT_ASSERT(!stranger.exists());
}

void Sqlite3BtProgressInfoFileTest::testCheckpointSurvivesTaskRowDeletion()
{
  // removeDownloadResult deletes the `task` row. That used to CASCADE into
  // task_progress and silently destroy the checkpoint of every errored
  // download, so a retry after a network drop could never resume.
  Sqlite3BtProgressInfoFile saver(dctx_, pieceStorage_, option_.get(),
                                  store_.get());
  saver.setBtRuntime(btRuntime_);
  saver.setPeerStorage(peerStorage_);
  saver.save();

  exec("DELETE FROM task");
  CPPUNIT_ASSERT(saver.exists());
}

void Sqlite3BtProgressInfoFileTest::testSaveUnderNewGidKeepsOneRowPerPath()
{
  Sqlite3BtProgressInfoFile first(dctx_, pieceStorage_, option_.get(),
                                  store_.get());
  first.setBtRuntime(btRuntime_);
  first.setPeerStorage(peerStorage_);
  first.save();

  auto next = rebind(0x1111222233334444ULL, "/path/to/file");
  bitfield_->setBit(9); // make the second save dirty
  Sqlite3BtProgressInfoFile second(next.dctx, pieceStorage_, option_.get(),
                                   store_.get());
  second.setBtRuntime(btRuntime_);
  second.setPeerStorage(peerStorage_);
  second.save();

  CPPUNIT_ASSERT_EQUAL(
      (int64_t)1,
      count("SELECT COUNT(*) FROM task_progress WHERE out_path = ?",
            "/path/to/file"));
  CPPUNIT_ASSERT_EQUAL(
      (int64_t)1, count("SELECT COUNT(*) FROM task_progress WHERE gid = ?",
                        GroupId::toHex(next.rg->getGID())));
}

void Sqlite3BtProgressInfoFileTest::testRemoveFileDropsCheckpointByPath()
{
  Sqlite3BtProgressInfoFile saver(dctx_, pieceStorage_, option_.get(),
                                  store_.get());
  saver.setBtRuntime(btRuntime_);
  saver.setPeerStorage(peerStorage_);
  saver.save();

  // A later gid finishing (or finding the data file gone) removes the path's
  // checkpoint, whichever gid wrote it.
  auto next = rebind(0x1111222233334444ULL, "/path/to/file");
  Sqlite3BtProgressInfoFile remover(next.dctx, pieceStorage_, option_.get(),
                                    store_.get());
  remover.removeFile();
  CPPUNIT_ASSERT(!remover.exists());
  CPPUNIT_ASSERT(!saver.exists());
}

void Sqlite3BtProgressInfoFileTest::testLegacyGidRowIsFoundAndReplaced()
{
  // Rows written before schema v3 carry no out_path. They stay reachable by
  // gid so an in-flight download survives the upgrade, and the first save
  // rewrites them into the path-addressed form.
  Sqlite3BtProgressInfoFile saver(dctx_, pieceStorage_, option_.get(),
                                  store_.get());
  saver.setBtRuntime(btRuntime_);
  saver.setPeerStorage(peerStorage_);
  saver.save();
  exec("UPDATE task_progress SET out_path = NULL, digest = X''");

  Sqlite3BtProgressInfoFile reopened(dctx_, pieceStorage_, option_.get(),
                                     store_.get());
  reopened.setBtRuntime(btRuntime_);
  reopened.setPeerStorage(peerStorage_);
  CPPUNIT_ASSERT(reopened.exists());
  reopened.save();

  auto gidHex = GroupId::toHex(rg_->getGID());
  CPPUNIT_ASSERT_EQUAL(
      (int64_t)1,
      count("SELECT COUNT(*) FROM task_progress WHERE gid = ?", gidHex));
  CPPUNIT_ASSERT_EQUAL(
      (int64_t)0,
      count("SELECT COUNT(*) FROM task_progress WHERE gid = ?"
            " AND out_path IS NULL",
            gidHex));
}

void Sqlite3BtProgressInfoFileTest::testPruneDropsOnlyDefunctOrphans()
{
  // Without the CASCADE nothing else reclaims a checkpoint whose download
  // was removed. Prune drops rows that no task owns AND whose data file is
  // gone; a present file is a resumable download, and a live task row keeps
  // its checkpoint regardless.
  std::string kept = std::string(A2_TEST_OUT_DIR) + "/prune-kept.bin";
  std::string gone = std::string(A2_TEST_OUT_DIR) + "/prune-gone.bin";
  std::string owned = std::string(A2_TEST_OUT_DIR) + "/prune-owned.bin";
  { std::ofstream(kept) << "partial"; }
  std::remove(gone.c_str());
  std::remove(owned.c_str());

  auto save = [&](a2_gid_t gid, const std::string& path) {
    auto r = rebind(gid, path);
    Sqlite3BtProgressInfoFile f(r.dctx, pieceStorage_, option_.get(),
                                store_.get());
    f.setBtRuntime(btRuntime_);
    f.setPeerStorage(peerStorage_);
    f.save();
  };
  save(0x0000000000000a01ULL, kept);
  save(0x0000000000000a02ULL, gone);
  // `owned` is still referenced by a live task row.
  exec("INSERT INTO task(gid, state, serialized, queue_position, digest,"
       " created_at, updated_at) VALUES ('" +
       GroupId::toHex(0x0000000000000a03ULL) +
       "', 'waiting', '', 1, X'', 0, 0)");
  save(0x0000000000000a03ULL, owned);

  Sqlite3BtProgressInfoFile::pruneDefunct(*store_);

  CPPUNIT_ASSERT_EQUAL(
      (int64_t)1,
      count("SELECT COUNT(*) FROM task_progress WHERE out_path = ?", kept));
  CPPUNIT_ASSERT_EQUAL(
      (int64_t)0,
      count("SELECT COUNT(*) FROM task_progress WHERE out_path = ?", gone));
  CPPUNIT_ASSERT_EQUAL(
      (int64_t)1,
      count("SELECT COUNT(*) FROM task_progress WHERE out_path = ?", owned));
  std::remove(kept.c_str());
}
#endif

} // namespace aria2

#endif // HAVE_SQLITE3
