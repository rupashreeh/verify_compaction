------------------------------ MODULE wal ------------------------------
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS CF, MaxWrites, DelayImm, MaxImm, BufStop, FixSeqAtAppend

VARIABLES Q, uid, imm, memUsed, log, nextSeq

vars == <<Q, uid, imm, memUsed, log, nextSeq>>

--------------------------------------------------------------------------

\* The two stall states
TStallState == IF \E c \in CF : imm[c] >= MaxImm THEN "Stop"
               ELSE IF \E c \in CF : imm[c] >= DelayImm THEN "Delay"
               ELSE "Go"

BStallState == IF BufStop > 0 /\ memUsed >= BufStop THEN "Stop" ELSE "Go"

Rec(q) == [kind |-> "W", s |-> q.s, c |-> q.c, id |-> q.id]

Init ==
      /\ Q = {}
      /\ uid = 0
      /\ imm = [c \in CF |-> 0]
      /\ memUsed = 0
      /\ log = <<>>
      /\ nextSeq = 1 
--------------------------------------------------------------------------
\*  WAL1 Admit
Admit(c, sync, disableWAL, noslowdown) ==
      /\ uid < MaxWrites
      /\ disableWAL => ~sync
      /\ LET u == TStallState
             v == BStallState
         IN  /\ \/ u = "Go" /\ v = "Go"
                \/ u = "Delay" /\ v = "Go" /\  ~noslowdown
      /\ uid' = uid + 1
      /\ Q' = Q \cup {[id |-> uid + 1, c |-> c, sync |-> sync, disableWAL |-> disableWAL,
                       st |-> "Admitted", s |-> IF FixSeqAtAppend THEN 0 ELSE nextSeq, lg |-> 0]}
      /\ nextSeq' = IF FixSeqAtAppend THEN nextSeq ELSE nextSeq + 1
      /\ UNCHANGED <<imm, memUsed, log>>

\*  WAL1 AppendGroup
AppendGroup(i) == \E q \in Q :
    /\ q.id = i
    /\ q \in Q /\ q.st = "Admitted"
    /\ LET s  == IF FixSeqAtAppend THEN nextSeq ELSE q.s
           q2 == [q EXCEPT !.s = s, !.lg = 1,
                           !.st = IF q.disableWAL THEN "Unlogged" ELSE "Appended"]
       IN  /\ Q' = (Q \ {q}) \cup {q2}
           /\ log' = IF q.disableWAL THEN log ELSE Append(log, Rec(q2))
    /\ nextSeq' = IF FixSeqAtAppend THEN nextSeq + 1 ELSE nextSeq
    /\ UNCHANGED <<uid, imm, memUsed>>

EnvFreeze(c) == /\ imm[c] < MaxImm
                /\ imm' = [imm EXCEPT ![c] = @ + 1]
                /\ UNCHANGED <<Q, uid, memUsed, log, nextSeq>>

EnvFlush(c) == /\ imm[c] > 0
               /\ imm' = [imm EXCEPT ![c] = @ - 1]
               /\ UNCHANGED <<Q, uid, memUsed, log, nextSeq>>

EnvMemUp ==  /\ memUsed < BufStop
             /\ memUsed' = memUsed + 1
             /\ UNCHANGED <<Q, uid, imm, log, nextSeq>>

EnvMemDown == /\ memUsed > 0
              /\ memUsed' = memUsed - 1
              /\ UNCHANGED <<Q, uid, imm, log, nextSeq>>

Next == \/ \E c \in CF, sync, disableWAL, noslowdown \in BOOLEAN : Admit(c, sync, disableWAL, noslowdown)
        \/ \E i \in 1..MaxWrites : AppendGroup(i)
        \/ \E c \in CF : EnvFlush(c) \/ EnvFreeze(c)
        \/ EnvMemUp \/ EnvMemDown

Spec == Init /\ [][Next]_vars
--------------------------------------------------------------------------

TypeOK ==
    /\ log \in Seq([kind : {"W"}, s : Nat, c : CF, id : Nat])
    /\ \A q \in Q : q.st \in {"Admitted", "Appended", "Unlogged"}

LogInOrder == \A i, j \in 1..Len(log) : i < j => log[i].s < log[j].s

LogMatchesQ == \A q \in Q : (q.st = "Appended") <=> Rec(q) \in {log[i] : i \in 1..Len(log)}

=============================================================================