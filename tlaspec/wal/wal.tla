------------------------------ MODULE wal ------------------------------

EXTENDS Naturals, Sequences 

CONSTANTS MAXWRITES,
          MAXLOGS

Logs == 1..MAXLOGS

VARIABLES logs, lstate, active, nextSeq 

vars == << logs, lstate, active, nextSeq >>

Init ==
       /\ logs = [l \in Logs |-> <<>>]
       /\ lstate = [l \in Logs |-> IF l = 1 THEN "Active" ELSE "Unused"]
       /\ active = 1
       /\ nextSeq = 1
       
AppendWrite == 
       /\ nextSeq <= MAXWRITES
       /\ logs' = [logs EXCEPT ![active] = Append(@, nextSeq)]
       /\ nextSeq' = nextSeq + 1 
       /\ UNCHANGED <<lstate, active>>


Next == AppendWrite 

Spec == Init /\ [] [Next]_vars 

RECURSIVE Concat(_)
Concat(n) == IF n = 0 THEN <<>> ELSE Concat(n - 1) \o logs[n]

LogOrdered == LET all == Concat(MAXLOGS) IN \A i \in 1..Len(all) : all[i] = i

OneActive == \A l \in Logs : (lstate[l] = "Active") <=> (l = active)

LogShape == \A l \in Logs :
              /\ l < active => lstate[l] = "Closed"
              /\ l > active => lstate[l] = "Unused" /\ logs[l] = <<>>

=============================================================================