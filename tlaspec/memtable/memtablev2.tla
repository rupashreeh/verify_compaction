------------------------------ MODULE memtable ------------------------------
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS CF, DefaultCF, Keys, MaxWrites, FixPublishAfterInserts


NoVal == <<"none">>

Min(S) == CHOOSE x \in S : \A y \in S : x <= y
Max(S) == CHOOSE x \in S : \A y \in S : y <= x
Rev(S) == [i \in 1..Len(S) |->  S[Len(S) - i + 1]]

VARIABLES mem, lastSeq, visSeq, clock, batches, hist, nWrites

vars == <<mem, lastSeq, visSeq, clock, batches, hist, nWrites>>

MkFile(E)     == [lo |-> Min({e.k : e \in E}), hi |-> Max({e.k : e \in E}), ents |-> E]
RangeOv(a, b) == a.lo <= b.hi /\ b.lo <= a.hi
KeyIn(E, lo, hi) == \E e \in E : lo <= e.k /\ e.k <= hi
LiveCFs     == CF
ReadableCFs == CF

MemGet(E, k, s) ==
    LET V == {e \in E : e.k = k /\ e.s <= s}
    IN  IF V = {} THEN NoVal ELSE (CHOOSE e \in V : \A e2 \in V : e2.s <= e.s).v

Comps(c) == <<mem[c]>>

Get(c, k, s) ==
    LET C == Comps(c)
        I == {i \in 1..Len(C) : MemGet(C[i], k, s) # NoVal}
    IN  IF I = {} THEN NoVal ELSE MemGet(C[Min(I)], k, s)

Expected(c, k, t) ==
    LET H == {h \in hist : h.c = c /\ h.k = k /\ h.t <= t}
    IN  IF H = {} THEN NoVal ELSE (CHOOSE h \in H : \A h2 \in H : h2.t <= h.t).v

Init ==
    /\ mem = [c \in CF |-> {}]
    /\ lastSeq = 0 /\ visSeq = 0 /\ clock = 0
    /\ batches = {} /\ hist = {} /\ nWrites = 0


Write(S, f) ==
    /\ nWrites < MaxWrites
    /\ batches' = batches \cup {[id |-> nWrites + 1, seq |-> lastSeq + 1, f |-> f, ins |-> {}]}
    /\ lastSeq' = lastSeq + 1
    /\ nWrites' = nWrites + 1
    /\ UNCHANGED <<mem, visSeq, clock, hist>>

Insert(b, c) ==
    /\ c \in DOMAIN b.f \ b.ins
    /\ mem' = [mem EXCEPT ![c] = @ \cup {[k |-> b.f[c], v |-> <<"w", b.id>>, s |-> b.seq]}]
    /\ batches' = (batches \ {b}) \cup {[b EXCEPT !.ins = @ \cup {c}]}
    /\ UNCHANGED <<lastSeq, visSeq, clock, hist, nWrites>>

Publish(b) ==
    /\ b.seq = visSeq + 1
    /\ FixPublishAfterInserts => b.ins = DOMAIN b.f
    /\ visSeq' = b.seq
    /\ clock' = clock + 1
    /\ hist' = hist \cup {[c |-> c, k |-> b.f[c], v |-> <<"w", b.id>>, t |-> clock + 1] :
                          c \in DOMAIN b.f}
    /\ UNCHANGED <<mem, lastSeq, batches, nWrites>>


Next ==
    \/ \E S \in (SUBSET LiveCFs) \ {{}} : \E f \in [S -> Keys] : Write(S, f)
    \/ \E b \in batches : \E c \in CF : Insert(b, c)
    \/ \E b \in batches : Publish(b)

Spec == Init /\ [][Next]_vars

\* Invariants
\* MS4: a published batch is in the memtable of every CF it writes.
BatchAtomic == \A b \in batches : b.seq <= visSeq => b.ins = DOMAIN b.f

\* Reachability checks: each "Reach" property is expected to FAIL; the trace
\* TLC prints is a run that reaches the situation.
ReachPublished == visSeq = 0



=============================================================================

