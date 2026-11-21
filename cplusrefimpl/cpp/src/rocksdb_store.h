// rocksdb_store.h: the RocksDBStore of rocksdb_store/ (DB1 to DB11) is the DB class.
//
// The write-up defines the store as one data structure. DB already owns every
// part of it, so this header only names the mapping:
//
//   Data structure RocksDBStore          C++
//     tree             : LSMTree         db.tree (memtables, versions, column families)
//     policy           : Policy          db.opts.compaction_style + LsmTree::PickLeveled / PickUniversal
//     config           : Configuration   db.opts (sanitized by the DB constructor, DB1)
//     job              : CompactionJob   db.tree.compactions (a map: RocksDB may run several)
//     wal              : WAL             db.wal
//     manifest         : Manifest        the MANIFEST file; db.tree.durableFiles, manifestLastSeq
//     bufferManager    : WBM             *db.tree.wbm (may be shared by several DBs)
//     blockCache       : BlockCache      *db.tree.blockCache (may be shared)
//     persistedThrough : Integer         db.persistedThrough
//     nextSeq          : N               db.tree.lastAllocated + 1
//     nextMemId        : N               db.tree.nextMemId
//     nextFileId       : N               db.tree.nextFileNumber
//     nextRunId        : N               db.tree.nextEpoch (L0 files and sorted runs are ordered by it)
//     accepting        : Boolean         db.accepting
//
// The steps:
//   DB1 Open                  DB::Open (WAL9 + CF4), then DeleteOrphanFiles
//   DB2 Write                 DB::Write / Put / Delete (WAL1 to WAL4b); DB::FlushWAL(true)
//   DB3 Get                   DB::Get, DB::NewIterators (MT7, CF5, BC9)
//   DB4 Flush                 DB::Flush, FlushStart / FlushFinish / InstallFlush (MT4 to MT6)
//   DB5 PickCompaction        DB::CompactStart (LSM5 or LSM6)
//   DB6 RunCompactionJob      DB::CompactWrite (LSM7, LSM8)
//   DB7 InstallCompaction     DB::CompactInstall (LSM9)
//   DB8 PurgeObsoleteFiles    DB::ReleaseVersion (obsolete list), DB::PurgeAllObsoleteFiles (full scan)
//   DB9 IngestExternalFile    DB::IngestExternalFiles (SST2 to SST4)
//   DB10 UpdateWriteStall     DB::UpdateWriteStall, LsmTree::GetStallState (LSM12)
//   DB11 Close                DB::Close
#pragma once
#include "db.h"

namespace lsm {
using RocksDBStore = DB;
}  // namespace lsm
