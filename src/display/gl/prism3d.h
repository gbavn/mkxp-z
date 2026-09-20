/*
** prism3d.h
**
** O passo 3D do Prism, dentro do mkxp-z.
**
** Malha de verdade, camera de perspectiva e buffer de profundidade, desenhados
** no mesmo contexto e no mesmo framebuffer que o compositor 2D do RGSS ja usa.
** Nada aqui e sprite, bitmap ou pre-renderizacao.
**
** O renderizador entra na cena como um SceneElement, que e a lista ordenada por
** z que o motor ja tem. Assim o 3D vale em todo caminho que compoe tela,
** transicao e fade inclusive, e o Ruby escolhe se o mundo 3D fica abaixo dos
** sprites, no meio ou acima.
**
** Duas regras que parecem exagero e nao sao:
**
** 1. `draw()` nao cria nem destroi nada. Tudo que e caro nasce em `init()`.
** 2. `draw()` devolve o estado de OpenGL exatamente como encontrou. O motor 2D
**    assume profundidade desligada, mistura ligada e nenhum VAO preso, e o
**    cache de estado dele (GLState) nem sequer conhece profundidade.
*/

#ifndef PRISM3D_H
#define PRISM3D_H

#include <string>
#include <vector>

#include "gl-meta.h"
#include "prism3d-math.h"
#include "scene.h"

namespace Prism3D {

/** Uma caixa no mundo. E o unico tipo de malha do primeiro marco. */
struct Box {
    Vec3 at;
    Vec3 size;
    /** Giro em volta do eixo vertical, em radianos. */
    float yaw;
    float red, green, blue;
};

/**
 * Uma malha vinda de arquivo, dividida por material.
 *
 * O OBJ separa a geometria em grupos, um por material, e cada material aponta
 * uma textura. Como trocar de textura obriga a trocar de desenho, o grupo e a
 * unidade natural: um buffer e uma chamada de desenho para cada.
 */
struct MeshGroup {
    GLMeta::VAO vao;
    VBO::ID vbo;
    IBO::ID ibo;
    int indexCount = 0;
    TEX::ID texture;
    bool textured = false;
    /* Cor do material, usada quando nao ha textura. */
    float red = 1.0f, green = 1.0f, blue = 1.0f;
};

class Mesh {
public:
    /**
     * Le um OBJ, o MTL ao lado dele e as texturas que ele pedir.
     *
     * `unitsPerTile` converte a escala do arquivo para celulas do mapa. Modelo
     * extraido de jogo de DS costuma vir com 16 unidades por tile.
     */
    bool load(const std::string &path, float unitsPerTile);
    void fini();

    std::vector<MeshGroup> groups;
};

/** Um modelo colocado no mundo. */
struct Placement {
    int mesh = -1;
    Vec3 at;
    float yaw = 0.0f;
    float scale = 1.0f;
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    /** Compila o shader e sobe a malha do cubo. Uma vez, fora do desenho. */
    bool init();
    void fini();
    bool ready() const { return program != 0; }

    void clear() { boxes.clear(); placements.clear(); }

    /** Carrega um modelo e devolve o indice dele, ou -1 se nao deu. */
    int loadMesh(const char *path, float unitsPerTile);
    void addPlacement(const Placement &placement) { placements.push_back(placement); }
    size_t meshCount() const { return meshes.size(); }
    void add(const Box &box) { boxes.push_back(box); }
    size_t count() const { return boxes.size(); }
    Box &at(size_t index) { return boxes[index]; }

    void setCamera(const Vec3 &eye, const Vec3 &target, float fovDegrees);

    /**
     * Passa a desenhar com a projecao do mapa, em vez da camera livre.
     *
     * `scrollX` e `scrollZ` sao o canto superior esquerdo visivel do mapa, em
     * tiles. `tilePixels` e o tamanho do tile na tela, 32 no RPG Maker XP, e
     * serve para descobrir quantos tiles cabem no alvo. `heightOnScreen` diz
     * quanto um tile de altura sobe na tela: 1 e a convencao dos tiles altos
     * do RPG Maker, valores menores achatam.
     */
    void setMapCamera(float scrollX, float scrollZ, float tilePixels,
                      float heightOnScreen);

    /**
     * Desenha, dentro do ciclo de desenho do motor.
     *
     * `width` e `height` sao os do alvo corrente, so para a proporcao da
     * camera. O viewport em si nao e tocado: mexer nele durante o desenho e
     * proibido pelo contrato de estado em scene.h.
     *
     * `clearDepth` diz se limpamos o buffer de profundidade antes. Isso
     * acontece uma vez por quadro, e com o teste de tesoura desligado, porque
     * glClear respeita tesoura e o ciclo de desenho deixa ela ligada.
     */
    void draw(int width, int height, bool clearDepth = true);

    /** Desenhar sem profundidade prova, no teste, que ela e quem ordena. */
    bool depthEnabled = true;

private:
    unsigned int program = 0;
    GLMeta::VAO vao;
    VBO::ID vbo;
    IBO::ID ibo;
    int indexCount = 0;

    int uniformModel = -1;
    int uniformViewProjection = -1;
    int uniformColor = -1;
    int uniformTextured = -1;

    Mat4 view = Mat4::identity();
    bool mapCamera = false;
    float mapScrollX = 0.0f, mapScrollZ = 0.0f;
    float mapTilePixels = 32.0f, mapHeight = 1.0f;
    float fov = 45.0f;
    Vec3 eye = Vec3(0, 3, 8);
    Vec3 target = Vec3(0, 0, 0);

    std::vector<Box> boxes;
    std::vector<Mesh *> meshes;
    std::vector<Placement> placements;

    /* Uma textura branca de um pixel, para o caminho sem textura usar o mesmo
       shader em vez de existir um segundo programa so por causa disso. */
    TEX::ID blank;

    void drawGroup(const MeshGroup &group, const Mat4 &model);
};

/**
 * O renderizador visto como elemento de cena.
 *
 * Fica na mesma lista de z dos sprites, entao o Ruby ordena 3D contra 2D pelo
 * mecanismo que o RGSS ja tem. Entre objetos 3D a profundidade e real, por
 * pixel; entre 3D e 2D a ordem e por z de elemento, porque os sprites do motor
 * nao escrevem profundidade. E o mesmo limite que o MV3D tem.
 */
class Element : public SceneElement {
public:
    Element(Scene &scene, int z);

    Renderer &renderer() { return theRenderer; }

    /* Nao ha nada preguicoso para resolver antes de ler propriedade daqui. */
    void aboutToAccess() const override {}

protected:
    void draw() override;
    void onGeometryChange(const Scene::Geometry &geo) override;

private:
    Renderer theRenderer;
    int width = 0, height = 0;
};

} // namespace Prism3D

#endif // PRISM3D_H
