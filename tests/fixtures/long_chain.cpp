// A definition chain longer than any hand-picked iteration cap.
//
// Three passes used to stop at a constant round count rather than at
// convergence. ValueProp was the worst: `for (int iter = 0; iter < 5; ++iter)`
// resolves exactly one link per round, so a 60-link chain kept four `double vN`
// declarations and a 120-link chain kept 63 -- and running `bin/cse` a second
// time finished the job, i.e. the output depended on how many times the pipeline
// ran. The value was still correct, so every numeric oracle passed.
//
// This fixture exists to be judged by a *shape*: run_tests.sh asserts the
// emitted body has collapsed to `return a + a;` with no residue declarations.
// A value check cannot see the difference between "fully inlined" and "stopped
// halfway", which is precisely why the defect survived.
//
// The chain is deliberately longer than 4 (the old LoopUnroll cap), 5 (the old
// ValueProp cap) and 10 (the old DCE/Cleanup caps), so any of those regressing
// back to a magic number shows up here.

//@cse
double sf_chain_60(double a) {
  double v0 = a;
  double v1 = v0;
  double v2 = v1;
  double v3 = v2;
  double v4 = v3;
  double v5 = v4;
  double v6 = v5;
  double v7 = v6;
  double v8 = v7;
  double v9 = v8;
  double v10 = v9;
  double v11 = v10;
  double v12 = v11;
  double v13 = v12;
  double v14 = v13;
  double v15 = v14;
  double v16 = v15;
  double v17 = v16;
  double v18 = v17;
  double v19 = v18;
  double v20 = v19;
  double v21 = v20;
  double v22 = v21;
  double v23 = v22;
  double v24 = v23;
  double v25 = v24;
  double v26 = v25;
  double v27 = v26;
  double v28 = v27;
  double v29 = v28;
  double v30 = v29;
  double v31 = v30;
  double v32 = v31;
  double v33 = v32;
  double v34 = v33;
  double v35 = v34;
  double v36 = v35;
  double v37 = v36;
  double v38 = v37;
  double v39 = v38;
  double v40 = v39;
  double v41 = v40;
  double v42 = v41;
  double v43 = v42;
  double v44 = v43;
  double v45 = v44;
  double v46 = v45;
  double v47 = v46;
  double v48 = v47;
  double v49 = v48;
  double v50 = v49;
  double v51 = v50;
  double v52 = v51;
  double v53 = v52;
  double v54 = v53;
  double v55 = v54;
  double v56 = v55;
  double v57 = v56;
  double v58 = v57;
  double v59 = v58;
  double v60 = v59;
  return v60 + v60;
}

double ref_chain_60(double a) {
  double v0 = a;
  double v1 = v0;
  double v2 = v1;
  double v3 = v2;
  double v4 = v3;
  double v5 = v4;
  double v6 = v5;
  double v7 = v6;
  double v8 = v7;
  double v9 = v8;
  double v10 = v9;
  double v11 = v10;
  double v12 = v11;
  double v13 = v12;
  double v14 = v13;
  double v15 = v14;
  double v16 = v15;
  double v17 = v16;
  double v18 = v17;
  double v19 = v18;
  double v20 = v19;
  double v21 = v20;
  double v22 = v21;
  double v23 = v22;
  double v24 = v23;
  double v25 = v24;
  double v26 = v25;
  double v27 = v26;
  double v28 = v27;
  double v29 = v28;
  double v30 = v29;
  double v31 = v30;
  double v32 = v31;
  double v33 = v32;
  double v34 = v33;
  double v35 = v34;
  double v36 = v35;
  double v37 = v36;
  double v38 = v37;
  double v39 = v38;
  double v40 = v39;
  double v41 = v40;
  double v42 = v41;
  double v43 = v42;
  double v44 = v43;
  double v45 = v44;
  double v46 = v45;
  double v47 = v46;
  double v48 = v47;
  double v49 = v48;
  double v50 = v49;
  double v51 = v50;
  double v52 = v51;
  double v53 = v52;
  double v54 = v53;
  double v55 = v54;
  double v56 = v55;
  double v57 = v56;
  double v58 = v57;
  double v59 = v58;
  double v60 = v59;
  return v60 + v60;
}
