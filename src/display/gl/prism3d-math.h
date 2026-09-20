/*
** prism3d-math.h
**
** Matriz 4x4 e vetor, o minimo para camera e transformacao.
**
** O mkxp-z nao tem tipo de matriz: ele e um compositor 2D e nunca precisou de
** uma. Procurei em src/etc/etc-internal.h e so existem Vec2, Vec4, Vec2i e
** retangulos. Entao a matriz vem daqui, e este arquivo e escrito para entrar
** no fork sem depender de nada: nem glm, nem Eigen, nem cabecalho de GL.
**
** Convencao: coluna-maior, como o OpenGL espera em glUniformMatrix4fv com
** transpose = GL_FALSE. O elemento (linha i, coluna j) fica em m[j * 4 + i].
*/

#ifndef PRISM3D_MATH_H
#define PRISM3D_MATH_H

#include <cmath>

namespace Prism3D {

struct Vec3 {
    float x, y, z;

    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vec3 operator-(const Vec3 &o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator+(const Vec3 &o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
};

inline Vec3 cross(const Vec3 &a, const Vec3 &b) {
    return Vec3(a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x);
}

inline float dot(const Vec3 &a, const Vec3 &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 normalize(const Vec3 &v) {
    const float length = std::sqrt(dot(v, v));
    return length > 0 ? v * (1.0f / length) : v;
}

struct Mat4 {
    float m[16];

    static Mat4 identity() {
        Mat4 out = {};
        out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.0f;
        return out;
    }

    static Mat4 translation(const Vec3 &at) {
        Mat4 out = identity();
        out.m[12] = at.x;
        out.m[13] = at.y;
        out.m[14] = at.z;
        return out;
    }

    static Mat4 scale(const Vec3 &by) {
        Mat4 out = identity();
        out.m[0] = by.x;
        out.m[5] = by.y;
        out.m[10] = by.z;
        return out;
    }

    /** Giro em volta do eixo vertical, em radianos. */
    static Mat4 rotationY(float radians) {
        Mat4 out = identity();
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        out.m[0] = c;
        out.m[2] = -s;
        out.m[8] = s;
        out.m[10] = c;
        return out;
    }

    static Mat4 rotationX(float radians) {
        Mat4 out = identity();
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        out.m[5] = c;
        out.m[6] = s;
        out.m[9] = -s;
        out.m[10] = c;
        return out;
    }

    /**
     * Projecao em perspectiva, no intervalo de profundidade de -1 a 1.
     *
     * E o intervalo do OpenGL de desktop e do GLES, que e onde o mkxp-z roda.
     */
    /*
     * Os planos se chamam nearPlane e farPlane, e nao near e far, porque no
     * Windows `near` e `far` sao MACROS do windef.h, herdadas da era dos
     * ponteiros segmentados de 16 bits, e expandem para nada. Com os nomes
     * curtos, `(far + near)` vira `( + )` e o compilador para ali. Fora do
     * Windows nao faria diferenca, mas o alvo aqui e justamente o Windows.
     */
    static Mat4 perspective(float fovYRadians, float aspect, float nearPlane, float farPlane) {
        Mat4 out = {};
        const float f = 1.0f / std::tan(fovYRadians / 2.0f);
        out.m[0] = f / aspect;
        out.m[5] = f;
        out.m[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
        out.m[11] = -1.0f;
        out.m[14] = (2.0f * farPlane * nearPlane) / (nearPlane - farPlane);
        return out;
    }

    static Mat4 orthographic(float left, float right, float bottom, float top,
                             float nearPlane, float farPlane) {
        Mat4 out = identity();
        out.m[0] = 2.0f / (right - left);
        out.m[5] = 2.0f / (top - bottom);
        out.m[10] = -2.0f / (farPlane - nearPlane);
        out.m[12] = -(right + left) / (right - left);
        out.m[13] = -(top + bottom) / (top - bottom);
        out.m[14] = -(farPlane + nearPlane) / (farPlane - nearPlane);
        return out;
    }

    /**
     * A projecao do mapa: obliqua, com o chao 1 para 1 com a tela.
     *
     * O mapa do RPG Maker nao tem fuga de ponto e nao comprime nada: um tile e
     * um quadrado de 32 por 32 pixels na tela, venha ele do topo ou do rodape
     * do mapa. Camera de verdade a 45 graus comprimiria a profundidade por
     * cos(45), e objetos ao sul iriam subindo em relacao aos tiles em que
     * pisam. Por isso aqui nao ha camera girada: ha cisalhamento.
     *
     * O mundo esta em tiles, com x para leste, z para o sul e y para cima. A
     * conta e esta:
     *
     *     tela_x = x - rolagemX
     *     tela_y = (z - rolagemZ) - y * alturaNaTela
     *
     * Com `alturaNaTela` igual a 1, um tile de altura sobe 32 pixels, que e a
     * convencao dos tiles altos do RPG Maker. Valores menores achatam o
     * objeto, como se a camera estivesse mais alta.
     *
     * A profundidade acompanha a direcao de projecao, que e (0, 1, alturaNaTela):
     * quanto mais ao sul e mais alto, mais perto do observador. `alcance` e a
     * profundidade total em tiles, e so precisa ser maior que o mapa.
     */
    static Mat4 mapOblique(float tilesWide, float tilesHigh,
                           float scrollX, float scrollZ,
                           float heightOnScreen, float range) {
        Mat4 out = {};

        const float sx = 2.0f / tilesWide;
        const float sy = 2.0f / tilesHigh;

        /*
         * Coluna-maior: m[coluna * 4 + linha]. As tres linhas sao
         *
         *   x = sx * (x - rolagemX) - 1
         *   y = 1 - sy * ((z - rolagemZ) - altura * y)
         *   z = -(y + altura * (z - rolagemZ)) / alcance
         */
        out.m[0]  = sx;                 /* linha 0, coluna x */
        out.m[5]  = sy * heightOnScreen;/* linha 1, coluna y */
        out.m[9]  = -sy;                /* linha 1, coluna z */
        out.m[6]  = -1.0f / range;      /* linha 2, coluna y */
        out.m[10] = -heightOnScreen / range;

        out.m[12] = -1.0f - sx * scrollX;
        out.m[13] = 1.0f + sy * scrollZ;
        out.m[14] = heightOnScreen * scrollZ / range;
        out.m[15] = 1.0f;

        return out;
    }

    /** Camera olhando de `eye` para `target`, com `up` definindo a inclinacao. */
    static Mat4 lookAt(const Vec3 &eye, const Vec3 &target, const Vec3 &up) {
        const Vec3 forward = normalize(target - eye);
        const Vec3 side = normalize(cross(forward, up));
        const Vec3 realUp = cross(side, forward);

        Mat4 out = identity();
        out.m[0] = side.x;    out.m[4] = side.y;    out.m[8]  = side.z;
        out.m[1] = realUp.x;  out.m[5] = realUp.y;  out.m[9]  = realUp.z;
        out.m[2] = -forward.x; out.m[6] = -forward.y; out.m[10] = -forward.z;
        out.m[12] = -dot(side, eye);
        out.m[13] = -dot(realUp, eye);
        out.m[14] = dot(forward, eye);
        return out;
    }
};

/** Produto de matrizes: aplica `b` e depois `a`, como o OpenGL espera. */
inline Mat4 operator*(const Mat4 &a, const Mat4 &b) {
    Mat4 out = {};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += a.m[k * 4 + row] * b.m[column * 4 + k];
            out.m[column * 4 + row] = sum;
        }
    }
    return out;
}

} // namespace Prism3D

#endif // PRISM3D_MATH_H
