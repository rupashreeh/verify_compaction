# The MANIFEST

The MANIFEST is a log file on disk that says what the RocksDB store is made of. After a crash, the
store reads it to rebuild the list of SST files in each level. The memtables are rebuilt from the
write-ahead log, not from the MANIFEST.

## What it holds

The MANIFEST is a list of records, oldest first. Each record is one change, called a version edit.
A record can say:

- which SST files to add to a level of a column family, and which to delete;
- a column family's new log number N_c, the oldest log it still needs;
- the last sequence number given out;
- the next file number;
- that a column family was created or dropped.

A small file called **CURRENT** names the MANIFEST file in use.

## Who writes it

| Step | Record |
|---|---|
| Flush (MT6) | add the new L0 file; move N_c forward |
| Compaction (LSM9) | delete the inputs; add the outputs |
| Ingestion (SST4) | add the ingested files; raise the last sequence number |
| Seal of an idle column family (WAL6a) | move its N_c forward |
| Create or drop a column family (CF1, CF2) | add or remove it |

## How a change is written

Every change goes through `LogAndApply(e)`:

1. Make sure every file that `e` adds is already synced to disk.
2. Append `e` to the MANIFEST as one record, with the last sequence number and the next file
   number.
3. Sync the MANIFEST.
4. Only then make the new list of files current in memory.

So a crash keeps either all of a change or none of it, and no file is deleted while the MANIFEST on
disk still lists it. These are contracts K4 and K15.

## How it is read back

At open (DB1), the store reads CURRENT and applies the MANIFEST's records in order. A torn last
record, cut short by a crash, is ignored. The result is the files of each level, each N_c, the last
sequence number, and the next file number. The store then sets the next file number past every file
on disk and replays the logs from the smallest N_c.

When the MANIFEST grows past `max_manifest_file_size`, the store starts a new one. Its first
record is a snapshot of the whole current state. Then CURRENT is switched to it in one atomic
rename. *(unchecked)*
