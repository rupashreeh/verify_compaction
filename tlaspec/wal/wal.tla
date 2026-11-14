------------------------------ MODULE wal ------------------------------

EXTENDS Naturals, Sequences 

CONSTANTS MAXWRITES 

VARIABLES log, nextSeq 

vars == << log, nextSeq >>

Init ==
       /\ log = << >>
       /\ nextSeq = 1
       
AppendWrite == 
       /\ nextSeq <= MAXWRITES
       /\ log' = Append(log, nextSeq)
       /\ nextSeq' = nextSeq + 1 

Next == AppendWrite 

Spec == Init /\ [] [Next]_vars 

LogOrdered == IF Len(log) = 0 THEN TRUE ELSE \A i \in 1..Len(log): log[i] = i
=============================================================================