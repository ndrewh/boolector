(benchmark smtaxiombvnor
 :logic QF_BV
 :extrafuns ((s BitVec[7]))
 :extrafuns ((t BitVec[7]))
 :formula (not (=
    (bvnor s t)  (bvnot (bvor s t))
)))
