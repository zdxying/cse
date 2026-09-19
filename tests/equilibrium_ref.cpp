// Hand-written CSE reference for D3Q19 equilibrium (mirrors equilibrium.ur.h).
// Used as a FLOP baseline for tests/equilibrium_d3q19.cpp.
//
//   feq[k] = w[k] * rho * (1 + 3*uc + 4.5*uc^2 - 1.5*u^2)
// Weights are grouped (w0 = 1/3, w1 = 1/18, w7 = 1/36), opposite directions
// share their uc^2 term, and the u^2 term is computed once.

//@cse
void equilibrium_d3q19_ref(double* feq, double rho, const double* u) {
    const double InvCs2x = 1.5;
    const double var0 = 1.0 - (u[0] * u[0] + u[1] * u[1] + u[2] * u[2]) * InvCs2x;
    const double rhowk0 = 0.3333333333333333 * rho;
    const double rhowk1 = 0.05555555555555555 * rho;
    const double rhowk7 = 0.02777777777777778 * rho;
    const double InvCs2uck1 = 3.0 * (u[0]);
    const double var0InvCs4uc2k1 = var0 + InvCs2uck1 * InvCs2uck1 * 0.5;
    const double InvCs2uck3 = 3.0 * (u[1]);
    const double var0InvCs4uc2k3 = var0 + InvCs2uck3 * InvCs2uck3 * 0.5;
    const double InvCs2uck5 = 3.0 * (u[2]);
    const double var0InvCs4uc2k5 = var0 + InvCs2uck5 * InvCs2uck5 * 0.5;
    const double InvCs2uck7 = 3.0 * (u[0] + u[1]);
    const double var0InvCs4uc2k7 = var0 + InvCs2uck7 * InvCs2uck7 * 0.5;
    const double InvCs2uck9 = 3.0 * (u[0] + u[2]);
    const double var0InvCs4uc2k9 = var0 + InvCs2uck9 * InvCs2uck9 * 0.5;
    const double InvCs2uck11 = 3.0 * (u[1] + u[2]);
    const double var0InvCs4uc2k11 = var0 + InvCs2uck11 * InvCs2uck11 * 0.5;
    const double InvCs2uck13 = 3.0 * (u[0] - u[1]);
    const double var0InvCs4uc2k13 = var0 + InvCs2uck13 * InvCs2uck13 * 0.5;
    const double InvCs2uck15 = 3.0 * (u[0] - u[2]);
    const double var0InvCs4uc2k15 = var0 + InvCs2uck15 * InvCs2uck15 * 0.5;
    const double InvCs2uck17 = 3.0 * (u[1] - u[2]);
    const double var0InvCs4uc2k17 = var0 + InvCs2uck17 * InvCs2uck17 * 0.5;
    feq[0] = rhowk0 * var0;
    feq[1] = rhowk1 * (var0InvCs4uc2k1 + InvCs2uck1);
    feq[2] = rhowk1 * (var0InvCs4uc2k1 - InvCs2uck1);
    feq[3] = rhowk1 * (var0InvCs4uc2k3 + InvCs2uck3);
    feq[4] = rhowk1 * (var0InvCs4uc2k3 - InvCs2uck3);
    feq[5] = rhowk1 * (var0InvCs4uc2k5 + InvCs2uck5);
    feq[6] = rhowk1 * (var0InvCs4uc2k5 - InvCs2uck5);
    feq[7] = rhowk7 * (var0InvCs4uc2k7 + InvCs2uck7);
    feq[8] = rhowk7 * (var0InvCs4uc2k7 - InvCs2uck7);
    feq[9] = rhowk7 * (var0InvCs4uc2k9 + InvCs2uck9);
    feq[10] = rhowk7 * (var0InvCs4uc2k9 - InvCs2uck9);
    feq[11] = rhowk7 * (var0InvCs4uc2k11 + InvCs2uck11);
    feq[12] = rhowk7 * (var0InvCs4uc2k11 - InvCs2uck11);
    feq[13] = rhowk7 * (var0InvCs4uc2k13 + InvCs2uck13);
    feq[14] = rhowk7 * (var0InvCs4uc2k13 - InvCs2uck13);
    feq[15] = rhowk7 * (var0InvCs4uc2k15 + InvCs2uck15);
    feq[16] = rhowk7 * (var0InvCs4uc2k15 - InvCs2uck15);
    feq[17] = rhowk7 * (var0InvCs4uc2k17 + InvCs2uck17);
    feq[18] = rhowk7 * (var0InvCs4uc2k17 - InvCs2uck17);
}
