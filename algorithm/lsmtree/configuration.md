## Configuration (θ)

The RocksDB store's options change how the WAL behaves. In the algorithms they are collected in one value, θ (theta). Each field below gives the RocksDB option it stands for, its default, and what it changes. Three more options are set per write, on each batch, rather than for the whole RocksDB store.

**Options for the whole RocksDB store**

- **θ.writeMode**: how writers share the work. The default is *Grouped*: the thread at the front of the queue handles a whole group of batches. `enable_pipelined_write` lets the next group start logging while this one goes into memory. `unordered_write` drops the ordering between groups. Used by WAL2, WAL4a and WAL4b.
- **θ.manualFlush** (`manual_wal_flush`, default off): if on, log bytes reach the operating system only when the user calls FlushWAL. Used by WAL2 and WAL3a.
- **θ.maxTotalWAL** (`max_total_wal_size`, default 0): the most log bytes kept before a flush is forced. If 0, the limit is 4 times the total memtable budget (`write_buffer_size` × `max_write_buffer_number`, summed over column families). Used by WAL1 and WAL5.
- **θ.atomicFlush** (`atomic_flush`, default off): if on, the chosen column families are flushed together and saved in one MANIFEST edit. Used by WAL5, WAL6b and WAL7b.
- **θ.wbm** (`db_write_buffer_size`, default 0, meaning no limit): the memory limit for all memtables together. With `allow_stall` (default off), writes also wait when memory is far over the limit. Used by WAL1.
- **θ.stall**: when to slow down or stop writes because background work is behind. `max_write_buffer_number` (default 2 memtables per column family), `level0_slowdown_writes_trigger` and `level0_stop_writes_trigger` (default 20 and 36 level-0 files), `soft_pending_compaction_bytes_limit` and `hard_pending_compaction_bytes_limit` (default 64 GB and 256 GB), and `delayed_write_rate` (the slowed rate, default 16 MB/s). Used by WAL1.
- **θ.dispose**: what happens to log files no longer needed. `WAL_ttl_seconds` and `WAL_size_limit_MB` (default 0, meaning no archive) keep them in an archive for that long or up to that size. `recycle_log_file_num` (default 0) keeps that many files for reuse as new logs. Used by WAL6a and WAL8b.
- **θ.mode** (`wal_recovery_mode`, default *PointInTime*): what recovery does with a damaged log record. *PointInTime* stops at the first gap. *TolerateTail* ignores a torn end. *SkipCorrupted* skips bad records. *Absolute* refuses to open. Used by WAL9.
- **θ.avoidRecoveryFlush** (`avoid_flush_during_recovery`, default off): if on, recovered data stays in memory instead of being flushed at the end of recovery. Used by WAL9.
- **θ.trackWALs** (`track_and_verify_wals_in_manifest`, default off): if on, the MANIFEST lists synced logs and their sizes, so recovery notices a missing or cut-short log. Used by WAL9.
- **θ.verifyPredecessor** (`track_and_verify_wals`, default off): if on, each new log records the last sequence number of the log before it, so recovery notices a missing log in the middle. Used by WAL6a and WAL9.
- **θ.compression** (`wal_compression`, default none): compresses log records, which changes their size on disk. Used by WAL2.

**Options set on each write**

- **sync** (default off): the client is answered only after its write is on disk, so it survives a power loss. Used by WAL2, WAL3b and WAL4b.
- **disableWAL** (default off): the batch skips the log. It is lost if the RocksDB store crashes before a flush. Used by WAL2.
- **no_slowdown** (default off): the batch fails at once instead of waiting when writes are stalled. Used by WAL1.
