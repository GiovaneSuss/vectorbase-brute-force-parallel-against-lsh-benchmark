#pragma once

// Distancia euclidiana AO QUADRADO entre dois vetores de dimensao d. A raiz quadrada e omitida porque nao
// muda a ordem dos vizinhos (sqrt e monotonica) e so custaria tempo.
//
// `omp simd reduction` permite ao compilador vetorizar a soma (varias parcelas ao mesmo tempo). No SIFT isso
// nao altera o resultado: os valores sao inteiros em [0,255], entao cada (a-b)^2 <= 65025 e a soma de 128
// termos <= 8.3M < 2^24 — todo valor intermediario e um inteiro exatamente representavel em float, e a soma
// da o mesmo resultado em qualquer ordem.
inline float l2_sq(const float* a, const float* b, int d) {
    float s = 0.0f;
#pragma omp simd reduction(+ : s)
    for (int j = 0; j < d; j++) {
        float diff = a[j] - b[j];
        s += diff * diff;
    }
    return s;
}
