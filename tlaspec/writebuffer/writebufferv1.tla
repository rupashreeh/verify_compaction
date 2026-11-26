------------------------------ MODULE writebuffer ------------------------------
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS DB, CF, WriteSizes, MaxWrites, MemCap

PAIRS == DB \X CF

Min(S) == CHOOSE x \in S : \A y \in S : x<=y 
Max(S) == CHOOSE x \in S : \A y \in S : x>=y  
MaxW == Max(MaxWrites)

RECURSIVE SumSeq(_)
SumSeq(q) == IF q = <<>> THEN 0 ELSE Head(q) + SumSeq(Tail(q))

RECURSIVE SumSet(_, _)
SumSet(f, S) == IF S = {} THEN 0
                ELSE LET x == CHOOSE x \in S : TRUE IN f[x] + SumSet(f, S \ {x})


VARIABLES used, active, mem, nWrites

vars == <<used, active, mem, nWrites >>

\* MT1 *\
Reserved(w) == 
    /\ used' = used + w 
    /\ active' = active + w

\* A write goes to a memtable that is not full and MT1 marks the bytes reserved for the write *\
\* WAL 4a with InsertGroup and MT1 *\ 
InsertGroup(d) == 
       /\ nWrites < MaxWrites
       /\ \E c \in CF, w \in WriteSizes :
            /\ mem [<<d,c>>] < MemCap
            /\ mem' = [mem EXCEPT ! [<<d,c>>] = @ + w]
            /\ Reserved(w)
       /\ nWrites' = nWrites + 1 
       /\ TRUE

Init ==
    /\ used = 0
    /\ active = 0
    /\ mem = [p \in PAIRS |-> 0]
    /\ nWrites = 0

Next ==
    \/ \E d \in DB : InsertGroup(d)

Spec == Init /\ [][Next]_vars  

RealActive == SumSet(mem, PAIRS)
RealUsed   == RealActive

ExactAccounting == used = RealUsed /\ active = RealActive

ReachFull == ~\E p \in PAIRS : mem[p] >= MemCap 

=============================================================================


