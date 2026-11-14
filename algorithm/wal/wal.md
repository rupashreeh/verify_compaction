** Reference ** 
https://github.com/meta/rocksdb/wiki/Write-Ahead-Log-%28WAL%29

** Problem **
A key–value store built as an LSM tree keeps its most recent writes in memory, in a sorted
structure called a memtable. When the process crashes or the machine loses power, every write 
the store has accepted but not yet copied to disk would be lost.

The write-ahead log fixes this by ensuring that before a write goes into the memtable, it
is appended to a log file on disk. From time to time a full memtable is written to disk as a sorted
file called an SST . Once every write in a log file is safely in an SST, thatlog file is no longer 
needed and can be deleted.

Compaction never touches the log directly, but the log decides when memtables are flushed, how big 
the resulting files are, and when writers are slowed down. Those decisions shape the work compaction 
receives, so the specification keeps every WAL behavior that changes them.

** Tracking the path of a single write **

(a) The client submits a write which waits in a queue. If the store is overloaded, it is slowed down or
rejected. Otherwise it is admitted (Algorithm WAL1)

(b) The write gets a sequence number, which fixes its place in the global order, and is appended to the 
current log file (Algorithm WAL2)

(c) The log bytes are pushed to the operating system and, if the client
asked for it, forced onto the disk (Algorithm WAL3a and WAL3b)

(d) The write is inserted into the memtable and then made visible to readers and the client receives the response.
(Algorithm WAL4a and WAL4b).

(e) When the memtable is full or the shared log has grown past its size limit, a flush is triggered (Algorithm WAL5).

(f) The memtable is frozen, a fresh one takes its place, and a new log file is started at the same moment (Algorithm WAL6a and WAL6b).

(g) A background job writes the frozen memtable to disk as an SST file.

(h) The SST is installed, and the WAL syncs the closed log files just before and records the progress just after (Algorithm WAL7a and WAL7b).

(i) Once no column family needs the old log file, it is marked obsolete and later deleted, archived, or recycled (Algorithm WAL8a and WAL8b).

(j) If the machine crashes at any point, the store replays the log files that are still needed to rebuild the memtables (Algorithm WAL9).