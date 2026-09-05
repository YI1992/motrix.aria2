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
#include "Sqlite3SessionStore.h"

#ifdef HAVE_SQLITE3

#include <cstdlib>
#include <set>
#include <vector>

#include <sqlite3.h>

#include <cppunit/extensions/HelperMacros.h>

#include "DownloadContext.h"
#include "Cookie.h"
#include "CookieStorage.h"
#include "FileEntry.h"
#include "GroupId.h"
#include "Option.h"
#include "RequestGroup.h"
#include "RequestGroupMan.h"
#include "Sqlite3PersistenceStore.h"
#include "TimeA2.h"
#include "TestUtil.h"
#include "prefs.h"

namespace aria2 {

class Sqlite3SessionStoreTest : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(Sqlite3SessionStoreTest);
  CPPUNIT_TEST(testSaveAllTasksUpsertsRows);
  CPPUNIT_TEST(testSaveLoadRoundTrip);
  CPPUNIT_TEST(testQueuePositionMove);
  CPPUNIT_TEST(testUpsertTaskInsertsThenUpdates);
  CPPUNIT_TEST(testTaskCookiesRoundTripIncludingEmpty);
  CPPUNIT_TEST(testTaskCookieReplacement);
  CPPUNIT_TEST(testDeleteTaskRemovesRow);
  CPPUNIT_TEST(testUpdateTaskState);
  CPPUNIT_TEST_SUITE_END();

private:
  std::shared_ptr<Option> option_;
  std::unique_ptr<Sqlite3PersistenceStore> store_;
  std::string dbPath_;

public:
  void setUp() override;
  void tearDown() override;
  void testSaveAllTasksUpsertsRows();
  void testSaveLoadRoundTrip();
  void testQueuePositionMove();
  void testUpsertTaskInsertsThenUpdates();
  void testTaskCookiesRoundTripIncludingEmpty();
  void testTaskCookieReplacement();
  void testDeleteTaskRemovesRow();
  void testUpdateTaskState();
};

CPPUNIT_TEST_SUITE_REGISTRATION(Sqlite3SessionStoreTest);

void Sqlite3SessionStoreTest::setUp()
{
  option_.reset(new Option());
  option_->put(PREF_DIR, A2_TEST_OUT_DIR);

  dbPath_ = std::string(A2_TEST_OUT_DIR) + "/sqlite3-session-store-test.db";
  std::remove(dbPath_.c_str());
  std::remove((dbPath_ + "-wal").c_str());
  std::remove((dbPath_ + "-shm").c_str());

  store_.reset(new Sqlite3PersistenceStore(dbPath_));
  store_->open();
}

void Sqlite3SessionStoreTest::tearDown()
{
  if (store_) {
    store_->finalCheckpointAndClose();
  }
  store_.reset();
  std::remove(dbPath_.c_str());
  std::remove((dbPath_ + "-wal").c_str());
  std::remove((dbPath_ + "-shm").c_str());
}

void Sqlite3SessionStoreTest::testSaveAllTasksUpsertsRows()
{
  // Build two reserved RequestGroups with downloadable URIs.
  auto makeRG = [&](const std::string& uri) {
    auto dctx = std::make_shared<DownloadContext>(0, 0, "");
    dctx->getFirstFileEntry()->addUri(uri);
    auto rg =
        std::make_shared<RequestGroup>(GroupId::create(), option_);
    rg->setDownloadContext(dctx);
    return rg;
  };

  auto rg1 = makeRG("http://example.com/file1.bin");
  auto rg2 = makeRG("http://example.com/file2.bin");

  RequestGroupMan rgman{std::vector<std::shared_ptr<RequestGroup>>(), 4,
                        option_.get()};
  rgman.addReservedGroup(rg1);
  rgman.addReservedGroup(rg2);

  Sqlite3SessionStore session(store_.get());
  session.saveAllTasks(&rgman);

  // Verify row count == 2.
  {
    sqlite3_stmt* stmt = nullptr;
    CPPUNIT_ASSERT_EQUAL(
        SQLITE_OK,
        sqlite3_prepare_v2(store_->raw(), "SELECT COUNT(*) FROM task", -1,
                           &stmt, nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    CPPUNIT_ASSERT_EQUAL(2, count);
  }

  // Verify queue_position values are exactly {0, 1} in two distinct rows.
  {
    sqlite3_stmt* stmt = nullptr;
    CPPUNIT_ASSERT_EQUAL(
        SQLITE_OK,
        sqlite3_prepare_v2(
            store_->raw(),
            "SELECT queue_position FROM task ORDER BY queue_position ASC", -1,
            &stmt, nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    CPPUNIT_ASSERT_EQUAL(0, sqlite3_column_int(stmt, 0));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    CPPUNIT_ASSERT_EQUAL(1, sqlite3_column_int(stmt, 0));
    CPPUNIT_ASSERT_EQUAL(SQLITE_DONE, sqlite3_step(stmt));
    sqlite3_finalize(stmt);
  }
}

void Sqlite3SessionStoreTest::testSaveLoadRoundTrip()
{
  auto makeRG = [&](const std::string& uri) {
    auto dctx = std::make_shared<DownloadContext>(0, 0, "");
    dctx->getFirstFileEntry()->addUri(uri);
    auto rg = std::make_shared<RequestGroup>(GroupId::create(), option_);
    rg->setDownloadContext(dctx);
    return rg;
  };

  a2_gid_t gid1, gid2;

  // Save phase: create RGs, persist them, then release all references so
  // the GroupId slots are freed before the load phase re-imports them.
  {
    auto rg1 = makeRG("http://example.com/file1.bin");
    auto rg2 = makeRG("http://example.com/file2.bin");

    gid1 = rg1->getGID();
    gid2 = rg2->getGID();

    RequestGroupMan rgman{std::vector<std::shared_ptr<RequestGroup>>(), 4,
                          option_.get()};
    rgman.addReservedGroup(rg1);
    rgman.addReservedGroup(rg2);

    Sqlite3SessionStore session(store_.get());
    session.saveAllTasks(&rgman);
  }
  // At this point rg1, rg2, and rgman are destroyed; GroupId slots freed.

  std::vector<std::shared_ptr<RequestGroup>> loaded;
  Sqlite3SessionStore session(store_.get());
  session.loadActiveTasksInto(loaded, option_);

  CPPUNIT_ASSERT_EQUAL((size_t)2, loaded.size());

  std::set<a2_gid_t> gotGids;
  for (const auto& rg : loaded) {
    gotGids.insert(rg->getGID());
  }

  std::set<a2_gid_t> wantGids{gid1, gid2};
  CPPUNIT_ASSERT(gotGids == wantGids);
}

namespace {

// Helper: directly INSERT a task row for tests that need pre-seeded data.
void insertTestRow(sqlite3* db, const std::string& gid, int pos,
                   const std::string& state = "waiting")
{
  std::string sql =
      "INSERT INTO task (gid, state, serialized, queue_position, digest,"
      " created_at, updated_at) VALUES ('" +
      gid + "', '" + state + "', '', " + std::to_string(pos) +
      ", X'', 0, 0)";
  char* errmsg = nullptr;
  int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errmsg);
  if (rc != SQLITE_OK) {
    std::string msg = errmsg ? errmsg : "unknown";
    sqlite3_free(errmsg);
    CPPUNIT_FAIL(("insertTestRow failed: " + msg).c_str());
  }
}

} // namespace

void Sqlite3SessionStoreTest::testQueuePositionMove()
{
  sqlite3* db = store_->raw();
  for (int i = 0; i < 5; ++i) {
    insertTestRow(db, "g" + std::to_string(i), i);
  }

  Sqlite3SessionStore session(store_.get());
  session.moveTaskPosition("g3", 0);

  sqlite3_stmt* stmt = nullptr;
  CPPUNIT_ASSERT_EQUAL(
      SQLITE_OK,
      sqlite3_prepare_v2(db, "SELECT gid FROM task ORDER BY queue_position ASC",
                         -1, &stmt, nullptr));

  std::vector<std::string> expected = {"g3", "g0", "g1", "g2", "g4"};
  for (const auto& want : expected) {
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    const char* got =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    CPPUNIT_ASSERT_EQUAL(want, std::string(got ? got : ""));
  }
  CPPUNIT_ASSERT_EQUAL(SQLITE_DONE, sqlite3_step(stmt));
  sqlite3_finalize(stmt);
}

void Sqlite3SessionStoreTest::testUpsertTaskInsertsThenUpdates()
{
  auto makeRG = [&](const std::string& uri) {
    auto dctx = std::make_shared<DownloadContext>(0, 0, "");
    dctx->getFirstFileEntry()->addUri(uri);
    auto rg = std::make_shared<RequestGroup>(GroupId::create(), option_);
    rg->setDownloadContext(dctx);
    return rg;
  };

  auto rg = makeRG("http://example.com/file1.bin");
  Sqlite3SessionStore session(store_.get());
  session.upsertTask(rg);

  sqlite3* db = store_->raw();

  {
    sqlite3_stmt* stmt = nullptr;
    CPPUNIT_ASSERT_EQUAL(
        SQLITE_OK,
        sqlite3_prepare_v2(
            db, "SELECT COUNT(*), queue_position, state FROM task", -1, &stmt,
            nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    CPPUNIT_ASSERT_EQUAL(1, sqlite3_column_int(stmt, 0));
    CPPUNIT_ASSERT_EQUAL(0, sqlite3_column_int(stmt, 1));
    const char* state =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    CPPUNIT_ASSERT_EQUAL(std::string("waiting"),
                         std::string(state ? state : ""));
    sqlite3_finalize(stmt);
  }

  int64_t createdAt = 0;
  {
    sqlite3_stmt* stmt = nullptr;
    CPPUNIT_ASSERT_EQUAL(
        SQLITE_OK,
        sqlite3_prepare_v2(db, "SELECT created_at FROM task", -1, &stmt,
                           nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    createdAt = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
  }

  rg->setPauseRequested(true);
  session.upsertTask(rg);

  {
    sqlite3_stmt* stmt = nullptr;
    CPPUNIT_ASSERT_EQUAL(
        SQLITE_OK,
        sqlite3_prepare_v2(
            db,
            "SELECT COUNT(*), queue_position, state, created_at FROM task", -1,
            &stmt, nullptr));
    CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
    CPPUNIT_ASSERT_EQUAL(1, sqlite3_column_int(stmt, 0));
    CPPUNIT_ASSERT_EQUAL(0, sqlite3_column_int(stmt, 1));
    const char* state =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    CPPUNIT_ASSERT_EQUAL(std::string("paused"),
                         std::string(state ? state : ""));
    CPPUNIT_ASSERT_EQUAL(createdAt, static_cast<int64_t>(sqlite3_column_int64(stmt, 3)));
    sqlite3_finalize(stmt);
  }
}

void Sqlite3SessionStoreTest::testTaskCookiesRoundTripIncludingEmpty()
{
  const auto now = Time().getTimeFromEpoch();
  a2_gid_t cookieGid;
  a2_gid_t emptyGid;
  {
    auto makeCookieRG = [&](const std::string& uri) {
      auto op = std::make_shared<Option>(*option_);
      op->put(PREF_REQUIRE_TASK_COOKIES, A2_V_TRUE);
      auto dctx = std::make_shared<DownloadContext>(0, 0, "");
      dctx->getFirstFileEntry()->addUri(uri);
      auto rg = std::make_shared<RequestGroup>(GroupId::create(), op);
      rg->setDownloadContext(dctx);
      return rg;
    };

    auto cookieRG = makeCookieRG("http://127.0.0.1/protected.bin");
    auto cookieStorage = std::make_shared<CookieStorage>();
    cookieStorage->store(make_unique<Cookie>(
                             "sid", "account-a", now + 3600, true,
                             "127.0.0.1", true, "/", false, true, now),
                         now);
    cookieRG->setTaskCookieStorage(cookieStorage);
    cookieGid = cookieRG->getGID();

    auto emptyRG = makeCookieRG("http://127.0.0.1/public.bin");
    emptyRG->setTaskCookieStorage(std::make_shared<CookieStorage>());
    emptyGid = emptyRG->getGID();

    Sqlite3SessionStore session(store_.get());
    session.upsertTask(cookieRG);
    session.upsertTask(emptyRG);
    const auto insertExpired =
        "INSERT INTO task_cookie"
        " SELECT gid, 'expired', 'stale', domain, path, host_only, secure,"
        " http_only, 1, 1, creation_time_unix_s, last_access_time_unix_s"
        " FROM task_cookie WHERE gid='" +
        GroupId::toHex(cookieGid) + "' LIMIT 1";
    CPPUNIT_ASSERT_EQUAL(
        SQLITE_OK,
        sqlite3_exec(store_->raw(), insertExpired.c_str(), nullptr, nullptr,
                     nullptr));
  }

  std::vector<std::shared_ptr<RequestGroup>> loaded;
  Sqlite3SessionStore session(store_.get());
  session.loadActiveTasksInto(loaded, option_);
  CPPUNIT_ASSERT_EQUAL((size_t)2, loaded.size());

  for (const auto& rg : loaded) {
    const auto& storage = rg->getTaskCookieStorage();
    CPPUNIT_ASSERT(storage);
    if (rg->getGID() == cookieGid) {
      auto cookies = storage->criteriaFind("127.0.0.1", "/protected.bin",
                                           now, false);
      CPPUNIT_ASSERT_EQUAL((size_t)1, cookies.size());
      CPPUNIT_ASSERT_EQUAL(std::string("sid"), cookies.front()->getName());
      CPPUNIT_ASSERT_EQUAL(std::string("account-a"),
                           cookies.front()->getValue());
      CPPUNIT_ASSERT(cookies.front()->getHttpOnly());
      sqlite3_stmt* stmt = nullptr;
      CPPUNIT_ASSERT_EQUAL(
          SQLITE_OK,
          sqlite3_prepare_v2(store_->raw(),
                             "SELECT COUNT(*) FROM task_cookie WHERE gid=?",
                             -1, &stmt, nullptr));
      const auto gidHex = GroupId::toHex(cookieGid);
      sqlite3_bind_text(stmt, 1, gidHex.c_str(), -1, SQLITE_STATIC);
      CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
      CPPUNIT_ASSERT_EQUAL(1, sqlite3_column_int(stmt, 0));
      sqlite3_finalize(stmt);
    }
    else {
      CPPUNIT_ASSERT_EQUAL(emptyGid, rg->getGID());
      CPPUNIT_ASSERT_EQUAL((size_t)0, storage->size());
    }
  }
}

void Sqlite3SessionStoreTest::testTaskCookieReplacement()
{
  auto op = std::make_shared<Option>(*option_);
  op->put(PREF_REQUIRE_TASK_COOKIES, A2_V_TRUE);
  auto dctx = std::make_shared<DownloadContext>(0, 0, "");
  dctx->getFirstFileEntry()->addUri("http://example.com/file.bin");
  auto rg = std::make_shared<RequestGroup>(GroupId::create(), op);
  rg->setDownloadContext(dctx);
  rg->setTaskCookieStorage(std::make_shared<CookieStorage>());

  Sqlite3SessionStore session(store_.get());
  session.upsertTask(rg);
  const auto gidHex = GroupId::toHex(rg->getGID());

  auto replacement = std::make_shared<CookieStorage>();
  const auto now = Time().getTimeFromEpoch();
  replacement->store(make_unique<Cookie>(
                         "sid", "replacement", 0, false, "example.com",
                         true, "/", false, false, now),
                     now);
  session.replaceTaskCookies(gidHex, replacement);

  sqlite3_stmt* stmt = nullptr;
  CPPUNIT_ASSERT_EQUAL(
      SQLITE_OK,
      sqlite3_prepare_v2(store_->raw(),
                         "SELECT value FROM task_cookie WHERE gid=?", -1,
                         &stmt, nullptr));
  sqlite3_bind_text(stmt, 1, gidHex.c_str(), -1, SQLITE_STATIC);
  CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
  const char* value =
      reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  CPPUNIT_ASSERT_EQUAL(std::string("replacement"),
                       std::string(value ? value : ""));
  sqlite3_finalize(stmt);

  session.replaceTaskCookies(gidHex, std::make_shared<CookieStorage>());
  CPPUNIT_ASSERT_EQUAL(
      SQLITE_OK,
      sqlite3_exec(store_->raw(),
                   ("UPDATE task_cookie_context SET updated_at=1 WHERE gid='" +
                    gidHex + "'")
                       .c_str(),
                   nullptr, nullptr, nullptr));
  session.upsertTask(rg, false);
  stmt = nullptr;
  CPPUNIT_ASSERT_EQUAL(
      SQLITE_OK,
      sqlite3_prepare_v2(
          store_->raw(),
          "SELECT (SELECT COUNT(*) FROM task_cookie_context WHERE gid=?),"
          " (SELECT COUNT(*) FROM task_cookie WHERE gid=?),"
          " (SELECT updated_at FROM task_cookie_context WHERE gid=?)",
          -1, &stmt, nullptr));
  sqlite3_bind_text(stmt, 1, gidHex.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, gidHex.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, gidHex.c_str(), -1, SQLITE_STATIC);
  CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
  CPPUNIT_ASSERT_EQUAL(1, sqlite3_column_int(stmt, 0));
  CPPUNIT_ASSERT_EQUAL(0, sqlite3_column_int(stmt, 1));
  CPPUNIT_ASSERT_EQUAL(1, sqlite3_column_int(stmt, 2));
  sqlite3_finalize(stmt);

  rg->setTaskCookieStorage(replacement);
  session.markTaskCookiesDirty(gidHex);
  session.upsertTask(rg, false);
  stmt = nullptr;
  CPPUNIT_ASSERT_EQUAL(
      SQLITE_OK,
      sqlite3_prepare_v2(store_->raw(),
                         "SELECT value FROM task_cookie WHERE gid=?", -1,
                         &stmt, nullptr));
  sqlite3_bind_text(stmt, 1, gidHex.c_str(), -1, SQLITE_STATIC);
  CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
  value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  CPPUNIT_ASSERT_EQUAL(std::string("replacement"),
                       std::string(value ? value : ""));
  sqlite3_finalize(stmt);

  session.deleteTaskCookies(gidHex);
  stmt = nullptr;
  CPPUNIT_ASSERT_EQUAL(
      SQLITE_OK,
      sqlite3_prepare_v2(store_->raw(),
                         "SELECT COUNT(*) FROM task_cookie_context", -1,
                         &stmt, nullptr));
  CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
  CPPUNIT_ASSERT_EQUAL(0, sqlite3_column_int(stmt, 0));
  sqlite3_finalize(stmt);
}

void Sqlite3SessionStoreTest::testDeleteTaskRemovesRow()
{
  sqlite3* db = store_->raw();
  insertTestRow(db, "abc", 0);

  Sqlite3SessionStore session(store_.get());
  session.replaceTaskCookies("abc", std::make_shared<CookieStorage>());
  session.deleteTask("abc");

  sqlite3_stmt* stmt = nullptr;
  CPPUNIT_ASSERT_EQUAL(
      SQLITE_OK,
      sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM task", -1, &stmt, nullptr));
  CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
  CPPUNIT_ASSERT_EQUAL(0, sqlite3_column_int(stmt, 0));
  sqlite3_finalize(stmt);

  CPPUNIT_ASSERT_EQUAL(
      SQLITE_OK,
      sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM task_cookie_context", -1,
                         &stmt, nullptr));
  CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
  CPPUNIT_ASSERT_EQUAL(0, sqlite3_column_int(stmt, 0));
  sqlite3_finalize(stmt);
}

void Sqlite3SessionStoreTest::testUpdateTaskState()
{
  sqlite3* db = store_->raw();
  insertTestRow(db, "abc", 0, "waiting");

  Sqlite3SessionStore session(store_.get());
  session.updateTaskState("abc", "paused");

  sqlite3_stmt* stmt = nullptr;
  CPPUNIT_ASSERT_EQUAL(
      SQLITE_OK,
      sqlite3_prepare_v2(db, "SELECT state FROM task WHERE gid='abc'", -1,
                         &stmt, nullptr));
  CPPUNIT_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
  const char* state =
      reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  CPPUNIT_ASSERT_EQUAL(std::string("paused"), std::string(state ? state : ""));
  sqlite3_finalize(stmt);
}

} // namespace aria2

#endif // HAVE_SQLITE3
