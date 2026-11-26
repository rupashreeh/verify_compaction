------------------------------ MODULE writebuffer ------------------------------
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS DB, CF, WriteSizes, MaxWrites, MemCap, MaxImm, AllowPins, FixScheduleFreeOnSeal, FixFreeAfterUnpin

Pairs == DB \X CF

Min(S) == CHOOSE x \in S : \A y \in S : x<=y 
Max(S) == CHOOSE x \in S : \A y \in S : x>=y  
MaxW == Max(MaxWrites)

RECURSIVE SumSeq(_)
SumSeq(q) == IF q = <<>> THEN 0 ELSE Head(q) + SumSeq(Tail(q))

RECURSIVE SumSet(_, _)
SumSet(f, S) == IF S = {} THEN 0
                ELSE LET x == CHOOSE x \in S : TRUE IN f[x] + SumSet(f, S \ {x})


VARIABLES used, active, mem, nWrites, imm, retired, freed

vars == <<used, active, mem, nWrites, imm, retired, freed >>

\* MT1 *\
Reserved(w) == 
    /\ used' = used + w 
    /\ active' = active + w

\*MT2 *\

ScheduleFreeMem(out) == 
          active' = IF FixScheduleFreeOnSeal THEN active - out ELSE active
FreeMem(m) ==
    /\ used'  = used - m
    /\ freed' = freed + 1

SwitchMemtable(d, S) ==
    LET out == SumSet([c \in S |-> mem[<<d, c>>]], S)
    IN  /\ imm' = [p \in Pairs |-> IF p[1] = d /\ p[2] \in S THEN Append(imm[p], mem[p]) ELSE imm[p]]
        /\ mem' = [p \in Pairs |-> IF p[1] = d /\ p[2] \in S THEN 0 ELSE mem[p]]
        /\ ScheduleFreeMem(out)

\* A write goes to a memtable that is not full and MT1 marks the bytes reserved for the write *\
\* WAL 4a with InsertGroup and MT1 *\ 
InsertGroup(d) == 
       /\ nWrites < MaxWrites
       /\ \E c \in CF, w \in WriteSizes :
            /\ mem [<<d,c>>] < MemCap
            /\ mem' = [mem EXCEPT ! [<<d,c>>] = @ + w]
            /\ Reserved(w)
       /\ nWrites' = nWrites + 1 
       /\ UNCHANGED << imm, retired, freed >> 

\* WAL1 sees a full memetable and calls WAL 6a
SealFull(p) ==
    /\ mem[p] >= MemCap
    /\ Len(imm[p]) < MaxImm
    /\ SwitchMemtable(p[1], {p[2]})
    /\ UNCHANGED <<used, nWrites, retired, freed>>


\* Background MT5 + MT6*\ 
InstallFlush(p) ==
    /\ imm[p] # <<>>
    /\ \E held \in (IF AllowPins THEN BOOLEAN ELSE {FALSE}) :
         LET m == Head(imm[p])
             early == ~FixFreeAfterUnpin
         IN  /\ imm' = [imm EXCEPT ![p] = Tail(@)]
             /\ retired' = [retired EXCEPT ![p[1]] = Append(@, [sz |-> m, held |-> held, counted |-> ~early])]
             /\ IF early THEN FreeMem(m) ELSE UNCHANGED <<used, freed>>
    /\ UNCHANGED <<active, mem, nWrites>>

\* A reader lets go of a flushed memtable.
Unpin(d) ==
    /\ \E i \in DOMAIN retired[d] :
         /\ retired[d][i].held
         /\ retired' = [retired EXCEPT ![d][i].held = FALSE]
    /\ UNCHANGED <<used, active, mem, nWrites, imm, freed>>

\* the memtable is destroyed and calls WBM.FreeMem (K7).
Destroy(d) ==
    /\ \E i \in DOMAIN retired[d] :
         /\ ~retired[d][i].held
         /\ retired' = [retired EXCEPT ![d] = SubSeq(@, 1, i - 1) \o SubSeq(@, i + 1, Len(@))]
         /\ IF retired[d][i].counted THEN FreeMem(retired[d][i].sz) ELSE UNCHANGED <<used, freed>>
    /\ UNCHANGED <<active, mem, nWrites, imm>>


Init ==
    /\ used = 0
    /\ active = 0
    /\ mem = [p \in Pairs |-> 0]
    /\ nWrites = 0
    /\ imm = [p \in Pairs |-> <<>>]
    /\ retired = [d \in DB |-> <<>>]
    /\ freed = 0

Next ==
    \/ \E d \in DB : InsertGroup(d) \/ Unpin(d) \/ Destroy(d)
    \/ \E p \in Pairs : SealFull(p) \/ InstallFlush(p)

Spec == Init /\ [][Next]_vars  

RealActive == SumSet(mem, Pairs)
RealUsed   == RealActive + SumSet([p \in Pairs |-> SumSeq(imm[p])], Pairs)
              + SumSet([d \in DB |-> SumSeq([i \in DOMAIN retired[d] |-> retired[d][i].sz])], DB)

ExactAccounting == used = RealUsed /\ active = RealActive

ReachFull == ~\E p \in Pairs : mem[p] >= MemCap 
ReachFreed == freed = 0

=============================================================================


