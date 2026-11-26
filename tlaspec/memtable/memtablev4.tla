------------------------------ MODULE memtable ------------------------------
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS CF, DefaultCF, Keys, MaxWrites, FixPublishAfterInserts, MemCap, MaxLogs,
          MaxImm,  AllowManualFlush, FixSealWaitsInserts          


NoVal == <<"none">>

Min(S) == CHOOSE x \in S : \A y \in S : x <= y
Max(S) == CHOOSE x \in S : \A y \in S : y <= x
Rev(S) == [i \in 1..Len(S) |->  S[Len(S) - i + 1]]

VARIABLES mem, lastSeq, visSeq, clock, batches, hist, imm, active, memLog, N, flushReq, nWrites

vars == <<mem, lastSeq, visSeq, clock, batches, hist, imm, active, memLog, N, flushReq, nWrites>>

MkFile(E)     == [lo |-> Min({e.k : e \in E}), hi |-> Max({e.k : e \in E}), ents |-> E]
RangeOv(a, b) == a.lo <= b.hi /\ b.lo <= a.hi
KeyIn(E, lo, hi) == \E e \in E : lo <= e.k /\ e.k <= hi
LiveCFs     == CF
ReadableCFs == CF
ImmEnts(c)  == UNION {imm[c][i].ents : i \in 1..Len(imm[c])}

\*MT2 *\
MemGet(E, k, s) ==
    LET V == {e \in E : e.k = k /\ e.s <= s}
    IN  IF V = {} THEN NoVal ELSE (CHOOSE e \in V : \A e2 \in V : e2.s <= e.s).v

Comps(c) == <<mem[c]>> \o [i \in 1..Len(imm[c]) |-> Rev(imm[c])[i].ents]

Get(c, k, s) ==
    LET C == Comps(c)
        I == {i \in 1..Len(C) : MemGet(C[i], k, s) # NoVal}
    IN  IF I = {} THEN NoVal ELSE MemGet(C[Min(I)], k, s)

Expected(c, k, t) ==
    LET H == {h \in hist : h.c = c /\ h.k = k /\ h.t <= t}
    IN  IF H = {} THEN NoVal ELSE (CHOOSE h \in H : \A h2 \in H : h2.t <= h.t).v

MemtableFull(c) == Cardinality(mem[c]) >= MemCap
IsEmpty(c) == mem[c] = {}
Pending(c) == Cardinality({b \in batches : c \in DOMAIN b.f \ b.ins})
Stalled == \E c \in LiveCFs : Len(imm[c]) >= MaxImm            

Init ==
    /\ mem = [c \in CF |-> {}]
    /\ lastSeq = 0 /\ visSeq = 0 /\ clock = 0
    /\ batches = {} /\ hist = {} /\ nWrites = 0
    /\ imm = [c \in CF |-> <<>>]
    /\ active = 1 /\ memLog = [c \in CF |-> 1] /\ N = [c \in CF |-> 1]
    /\ flushReq = {}

\* write path of wal *\
Write(S, f) ==
    /\ nWrites < MaxWrites
    /\ ~Stalled                           \* (step 4)
    /\ batches' = batches \cup {[id |-> nWrites + 1, seq |-> lastSeq + 1, f |-> f, ins |-> {}]}
    /\ lastSeq' = lastSeq + 1
    /\ nWrites' = nWrites + 1
    /\ UNCHANGED <<mem, visSeq, clock, hist, imm, active, memLog, N, flushReq>>


Insert(b, c) ==
    /\ c \in DOMAIN b.f \ b.ins
    /\ mem' = [mem EXCEPT ![c] = @ \cup {[k |-> b.f[c], v |-> <<"w", b.id>>, s |-> b.seq]}]
    /\ batches' = (batches \ {b}) \cup {[b EXCEPT !.ins = @ \cup {c}]}
    /\ UNCHANGED <<lastSeq, visSeq, clock, hist, imm, active, memLog, N, flushReq, nWrites>>

Publish(b) ==
    /\ b.seq = visSeq + 1
    /\ FixPublishAfterInserts => b.ins = DOMAIN b.f
    /\ visSeq' = b.seq
    /\ clock' = clock + 1
    /\ hist' = hist \cup {[c |-> c, k |-> b.f[c], v |-> <<"w", b.id>>, t |-> clock + 1] :
                          c \in DOMAIN b.f}
    /\ UNCHANGED <<mem, lastSeq, batches, imm, active, memLog, N, flushReq, nWrites>>

SealMutable(seal, n) == FALSE

Idle(d) == mem[d] = {} /\ imm[d] = <<>> /\ ~\E b \in batches : d \in DOMAIN b.f \ b.ins
SwitchMemtable(S) ==
    LET n    == active + 1
        seal == {c \in S : c \in LiveCFs /\ mem[c] # {}}
    IN
    /\ seal # {} /\ active < MaxLogs
    /\ \A c \in seal : Len(imm[c]) < MaxImm               \* a bound for the model only
    /\ FixSealWaitsInserts => \A b \in batches : b.ins = DOMAIN b.f
    /\ SealMutable(seal, n)
    /\ active' = n
    /\ memLog' = [d \in CF |-> IF d \in seal \/ Idle(d) THEN n ELSE memLog[d]]
    /\ N' = [d \in CF |-> IF Idle(d) /\ d \in LiveCFs THEN Max({N[d], n}) ELSE N[d]]
    /\ flushReq' = flushReq \cup seal
    /\ UNCHANGED <<lastSeq, visSeq, clock, batches, hist, nWrites>>

\* WAL1: c's memtable is full (MT3).
WAL1_MemtableFull(c) == MemtableFull(c) /\ SwitchMemtable({c})
\* The user calls Flush(c).
ManualFlush(c) == AllowManualFlush /\ SwitchMemtable({c})

Next ==
    \/ \E S \in (SUBSET LiveCFs) \ {{}} : \E f \in [S -> Keys] : Write(S, f)
    \/ \E b \in batches : \E c \in CF : Insert(b, c)
    \/ \E b \in batches : Publish(b)
    \/ \E c \in CF : WAL1_MemtableFull(c) \/ ManualFlush(c)

Spec == Init /\ [][Next]_vars


\* Invariants
\* MS4: a published batch is in the memtable of every CF it writes.
BatchAtomic == \A b \in batches : b.seq <= visSeq => b.ins = DOMAIN b.f
\* MS1: a read at the last published sequence number returns the latest write.
ReadLatest == \A c \in ReadableCFs : \A k \in Keys : Get(c, k, visSeq) = Expected(c, k, clock)
\* MS3: along the read path, a later place never holds a newer version.
NewerShadowsOlder ==
    \A c \in ReadableCFs : LET C == Comps(c) IN
        \A i, j \in 1..Len(C) : i < j =>
            \A e \in C[i], e2 \in C[j] : e.k = e2.k => e2.s <= e.s

\* Reachability checks: each "Reach" property is expected to FAIL; the trace
\* TLC prints is a run that reaches the situation.
ReachPublished == visSeq = 0
ReachFull == \A c \in CF : ~MemtableFull(c)
ReachFrozen == \A c \in CF : imm[c] = <<>>



=============================================================================

