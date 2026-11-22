------------------------------ MODULE wal ------------------------------
EXTENDS Naturals, FiniteSets

CONSTANTS CF, MaxWrites, DelayImm, MaxImm, BufStop, FixNoSlowdown

VARIABLES Q, uid, imm, memUsed, seen

vars == <<Q, uid, imm, memUsed, seen>>

--------------------------------------------------------------------------

\* The two stall states
TStallState == IF \E c \in CF : imm[c] >= MaxImm THEN "Stop"
               ELSE IF \E c \in CF : imm[c] >= DelayImm THEN "Delay"
               ELSE "Go"

BStallState == IF BufStop > 0 /\ memUsed >= BufStop THEN "Stop" ELSE "Go"

Init ==
      /\ Q = {}
      /\ uid = 0
      /\ imm = [c \in CF |-> 0]
      /\ memUsed = 0
      /\ seen = {}
--------------------------------------------------------------------------

Admit(c, sync, disableWAL, noslowdown) ==
      /\ uid < MaxWrites
      /\ disableWAL => ~sync
      /\ LET u == TStallState
             v == BStallState
         IN  /\ \/ u = "Go" /\ v = "Go"
                \/ u = "Delay" /\ v = "Go" /\ (FixNoSlowdown => ~noslowdown)
             /\ seen' = seen \cup {[ns |-> noslowdown, u |-> u, v |-> v]}
      /\ uid' = uid + 1
      /\ Q' = Q \cup {[id |-> uid + 1, c |-> c, sync |-> sync, disableWAL |-> disableWAL,
                       st |-> "Admitted", s |-> 0, lg |-> 0]}
      /\ UNCHANGED <<imm, memUsed>>

EnvFreeze(c) == /\ imm[c] < MaxImm
                /\ imm' = [imm EXCEPT ![c] = @ + 1]
                /\ UNCHANGED <<Q, uid, memUsed, seen>>

EnvFlush(c) == /\ imm[c] > 0
               /\ imm' = [imm EXCEPT ![c] = @ - 1]
               /\ UNCHANGED <<Q, uid, memUsed, seen>>

EnvMemUp ==  /\ memUsed < BufStop
             /\ memUsed' = memUsed + 1
             /\ UNCHANGED <<Q, uid, imm, seen>>

EnvMemDown == /\ memUsed > 0
              /\ memUsed' = memUsed - 1
              /\ UNCHANGED <<Q, uid, imm, seen>>

Next == \/ \E c \in CF, sync, disableWAL, noslowdown \in BOOLEAN : Admit(c, sync, disableWAL, noslowdown)
        \/ \E c \in CF : EnvFlush(c) \/ EnvFreeze(c)
        \/ EnvMemUp \/ EnvMemDown

Spec == Init /\ [][Next]_vars
--------------------------------------------------------------------------

TypeOK ==
       /\ uid \in 0..MaxWrites
       /\ imm \in [CF -> 0..MaxImm]
       /\ \A q \in Q : q.st = "Admitted"

NeverAdmitUnderStop == \A x \in seen : x.u # "Stop" /\ x.v # "Stop"

NoSlowdownNeverWaits == \A x \in seen : x.ns => (x.u = "Go" /\ x.v = "Go")

UniqueIds == \A p, q \in Q : p.id = q.id => p = q

=============================================================================