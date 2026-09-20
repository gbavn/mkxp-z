/*
** prism3d.cpp
**
** Implementacao do passo 3D. A logica e a mesma do prototipo em
** tools/prism3d-prototype/ do repositorio do Prism, que foi escrito e provado
** fora do motor justamente para nao pagar o custo de compilar o mkxp-z a cada
** tentativa. A diferenca aqui: as chamadas de OpenGL passam pela tabela `gl`
** do motor, e o estado que o motor cacheia em GLState vai por push/pop em vez
** de na mao.
*/

#include "prism3d.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

#include <SDL_image.h>
#include <SDL_rwops.h>
#include <cstring>

#include "gl-fun.h"
#include "gl-util.h"
#include "glstate.h"
#include "graphics.h"
#include "shader.h"
#include "sharedstate.h"
#include "vertex.h"
#include "exception.h"
#include "filesystem.h"
#include "prism-trace.h"

namespace Prism3D {

/*
** Os dois shaders, embutidos.
**
** Embutidos e nao em shader/ porque os shaders do motor passam por um gerador
** que transforma arquivo em array de bytes no build; entrar nele significaria
** mexer em shader/meson.build e no gerador. Sao trinta linhas, e assim o patch
** no build fica em duas linhas.
**
** A versao pedida e a 100 do GLSL ES, que e o minimo que roda tanto no OpenGL
** de desktop quanto no GLES por ANGLE, que e o caminho do macOS com Apple
** Silicon. Nada aqui usa recurso mais novo que isso.
*/
static const char *vertexSource = R"(
attribute vec3 position;
attribute vec3 normal;
attribute vec2 uv;

uniform mat4 model;
uniform mat4 viewProjection;

varying vec3 vNormal;
varying vec2 vUv;

void main() {
    vNormal = mat3(model[0].xyz, model[1].xyz, model[2].xyz) * normal;
    vUv = uv;
    gl_Position = viewProjection * model * vec4(position, 1.0);
}
)";

static const char *fragmentSource = R"(
#ifdef GLSLES
precision mediump float;
#endif

uniform vec3 color;
uniform sampler2D tex;
uniform float textured;

varying vec3 vNormal;
varying vec2 vUv;

void main() {
    /* Iluminacao de uma luz so, fixa. Serve para as faces se distinguirem:
       sem isso o cubo vira uma silhueta chapada e ninguem consegue dizer se
       ele girou. */
    vec3 light = normalize(vec3(-0.4, 0.9, 0.55));
    float lambert = max(dot(normalize(vNormal), light), 0.0);

    vec4 amostra = texture2D(tex, vUv);
    vec3 base = mix(color, color * amostra.rgb, textured);
    float alpha = mix(1.0, amostra.a, textured);

    /* Recorte em vez de mistura: textura de jogo de DS usa transparencia
       binaria, e misturar exigiria ordenar por profundidade a cada quadro. */
    if (alpha < 0.5)
        discard;

    gl_FragColor = vec4(base * (0.45 + 0.55 * lambert), 1.0);
}
)";

static GLuint compile(GLenum type, const char *source) {
    const GLuint shader = gl.CreateShader(type);

    /* O mesmo define que o motor passa aos shaders dele, para o fragmento
       saber se precisa declarar precisao. */
    static const char glesDefine[] = "#define GLSLES\n";
    const GLchar *parts[2];
    GLint sizes[2];
    size_t count = 0;

    if (gl.glsles) {
        parts[count] = glesDefine;
        sizes[count] = sizeof(glesDefine) - 1;
        ++count;
    }
    parts[count] = source;
    sizes[count] = (GLint)std::strlen(source);
    ++count;

    gl.ShaderSource(shader, (GLsizei)count, parts, sizes);
    gl.CompileShader(shader);

    GLint ok = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        gl.GetShaderInfoLog(shader, sizeof(log) - 1, 0, log);
        std::fprintf(stderr, "Prism3D: shader nao compilou: %s\n", log);
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}


/*
** Leitura de OBJ, do MTL ao lado dele e das texturas que ele pedir.
**
** O OBJ e texto e o subconjunto que interessa e pequeno: posicoes, coordenadas
** de textura, normais, faces e troca de material. Nao ha animacao, esqueleto
** nem cena: e geometria parada, que e exatamente o que um predio de mapa e.
**
** Face de OBJ pode ter mais de tres cantos, e modelo extraido de jogo de DS
** costuma vir em quadrilateros. Eles viram leque de triangulos, que e correto
** para face convexa e e o que qualquer visualizador de OBJ faz.
**
** O arquivo indexa posicao, textura e normal separadamente, e o OpenGL so
** aceita um indice por vertice. Entao cada combinacao distinta vira um vertice
** proprio, com um mapa para nao duplicar o que ja apareceu.
*/

struct ObjVertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

struct ObjMaterial {
    std::string texture;
    float red = 1.0f, green = 1.0f, blue = 1.0f;
};

/* Le um arquivo inteiro pelo sistema de arquivos do motor, que e quem conhece
   as pastas e os arquivos empacotados do projeto. */
static bool readWholeFile(const std::string &path, std::string &out) {
    SDL_RWops ops;
    try {
        shState->fileSystem().openReadRaw(ops, path.c_str(), true);
    } catch (const Exception &e) {
        prismTrace((std::string("OBJ: nao abriu ") + path).c_str());
        return false;
    }

    const Sint64 size = SDL_RWsize(&ops);
    if (size <= 0) {
        SDL_RWclose(&ops);
        return false;
    }

    out.resize((size_t)size);
    const size_t lido = SDL_RWread(&ops, &out[0], 1, (size_t)size);
    SDL_RWclose(&ops);
    out.resize(lido);

    return lido > 0;
}

static std::string directoryOf(const std::string &path) {
    const size_t corte = path.find_last_of("/\\");
    return corte == std::string::npos ? std::string() : path.substr(0, corte + 1);
}

/* Tira espaco e retorno de carro das pontas: arquivo gerado no Windows traz
   \r no fim e isso estraga comparacao de nome de material. */
static std::string trimmed(const std::string &s) {
    size_t inicio = 0;
    size_t fim = s.size();
    while (inicio < fim && (s[inicio] == ' ' || s[inicio] == '\t')) ++inicio;
    while (fim > inicio) {
        const char c = s[fim - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') --fim; else break;
    }
    return s.substr(inicio, fim - inicio);
}

static void parseMTL(const std::string &path,
                     std::map<std::string, ObjMaterial> &out) {
    std::string texto;
    if (!readWholeFile(path, texto))
        return;

    std::istringstream entrada(texto);
    std::string linha;
    std::string atual;

    while (std::getline(entrada, linha)) {
        std::istringstream campos(trimmed(linha));
        std::string chave;
        campos >> chave;

        if (chave == "newmtl") {
            std::string nome;
            campos >> nome;
            atual = trimmed(nome);
            out[atual];
        } else if (atual.empty()) {
            continue;
        } else if (chave == "map_Kd") {
            /* O resto da linha, porque nome de textura pode ter espaco. */
            std::string resto;
            std::getline(campos, resto);
            out[atual].texture = trimmed(resto);
        } else if (chave == "Kd") {
            float r = 1, g = 1, b = 1;
            campos >> r >> g >> b;
            out[atual].red = r;
            out[atual].green = g;
            out[atual].blue = b;
        }
    }
}

static TEX::ID loadTexture(const std::string &path, bool &ok) {
    ok = false;
    TEX::ID id(0);

    SDL_RWops ops;
    try {
        shState->fileSystem().openReadRaw(ops, path.c_str(), true);
    } catch (const Exception &e) {
        prismTrace((std::string("OBJ: textura ausente ") + path).c_str());
        return id;
    }

    const size_t ponto = path.find_last_of('.');
    const std::string ext = ponto == std::string::npos ? "png" : path.substr(ponto + 1);

    SDL_Surface *img = IMG_LoadTyped_RW(&ops, 1, ext.c_str());
    if (!img) {
        prismTrace((std::string("OBJ: textura nao decodificou ") + path).c_str());
        return id;
    }

    SDL_Surface *rgba = img;
    if (img->format->format != SDL_PIXELFORMAT_ABGR8888) {
        rgba = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_ABGR8888, 0);
        SDL_FreeSurface(img);
        if (!rgba)
            return id;
    }

    id = TEX::gen();
    TEX::bind(id);
    TEX::uploadImage(rgba->w, rgba->h, rgba->pixels, GL_RGBA);
    /* Textura de DS e minuscula e feita para repetir; filtro suave so borraria
       um desenho de 8 por 8 pixels. */
    TEX::setSmooth(false);
    TEX::setRepeat(true);
    SDL_FreeSurface(rgba);

    ok = true;
    return id;
}

bool Mesh::load(const std::string &path, float unitsPerTile) {
    std::string texto;
    if (!readWholeFile(path, texto))
        return false;

    const std::string pasta = directoryOf(path);
    const float escala = unitsPerTile > 0.0f ? 1.0f / unitsPerTile : 1.0f;

    std::vector<float> posicoes, uvs, normais;
    std::map<std::string, ObjMaterial> materiais;

    /* Por material: vertices ja montados e indices. */
    std::map<std::string, std::vector<ObjVertex> > vertPorMat;
    std::map<std::string, std::vector<unsigned short> > idxPorMat;
    std::map<std::string, std::map<std::string, unsigned short> > vistosPorMat;

    std::string materialAtual = "";
    std::istringstream entrada(texto);
    std::string linha;

    while (std::getline(entrada, linha)) {
        const std::string limpa = trimmed(linha);
        if (limpa.empty() || limpa[0] == '#')
            continue;

        std::istringstream campos(limpa);
        std::string chave;
        campos >> chave;

        if (chave == "v") {
            float x, y, z;
            campos >> x >> y >> z;
            posicoes.push_back(x * escala);
            posicoes.push_back(y * escala);
            posicoes.push_back(z * escala);
        } else if (chave == "vt") {
            float u = 0, v = 0;
            campos >> u >> v;
            uvs.push_back(u);
            /* OBJ conta a textura de baixo para cima, OpenGL de cima para
               baixo; sem inverter, tudo sai de cabeca para baixo. */
            uvs.push_back(1.0f - v);
        } else if (chave == "vn") {
            float x, y, z;
            campos >> x >> y >> z;
            normais.push_back(x);
            normais.push_back(y);
            normais.push_back(z);
        } else if (chave == "mtllib") {
            std::string resto;
            std::getline(campos, resto);
            parseMTL(pasta + trimmed(resto), materiais);
        } else if (chave == "usemtl") {
            std::string nome;
            campos >> nome;
            materialAtual = trimmed(nome);
        } else if (chave == "f") {
            std::vector<unsigned short> cantos;
            std::string pedaco;

            while (campos >> pedaco) {
                pedaco = trimmed(pedaco);

                std::map<std::string, unsigned short> &vistos = vistosPorMat[materialAtual];
                std::map<std::string, unsigned short>::iterator achado = vistos.find(pedaco);

                if (achado != vistos.end()) {
                    cantos.push_back(achado->second);
                    continue;
                }

                /* v, v/vt, v//vn ou v/vt/vn. Indice negativo conta do fim. */
                int iv = 0, it = 0, in = 0;
                {
                    std::string campo = pedaco;
                    for (size_t i = 0; i < campo.size(); ++i)
                        if (campo[i] == '/') campo[i] = ' ';

                    std::istringstream partes(campo);
                    const size_t barras = std::count(pedaco.begin(), pedaco.end(), '/');
                    if (barras == 0) {
                        partes >> iv;
                    } else if (barras == 1) {
                        partes >> iv >> it;
                    } else if (pedaco.find("//") != std::string::npos) {
                        partes >> iv >> in;
                    } else {
                        partes >> iv >> it >> in;
                    }
                }

                ObjVertex vert = {};
                const int totalV = (int)(posicoes.size() / 3);
                const int totalT = (int)(uvs.size() / 2);
                const int totalN = (int)(normais.size() / 3);

                const int ip = iv > 0 ? iv - 1 : totalV + iv;
                if (ip >= 0 && ip < totalV) {
                    vert.x = posicoes[ip * 3];
                    vert.y = posicoes[ip * 3 + 1];
                    vert.z = posicoes[ip * 3 + 2];
                }

                if (it != 0) {
                    const int iu = it > 0 ? it - 1 : totalT + it;
                    if (iu >= 0 && iu < totalT) {
                        vert.u = uvs[iu * 2];
                        vert.v = uvs[iu * 2 + 1];
                    }
                }

                if (in != 0) {
                    const int ino = in > 0 ? in - 1 : totalN + in;
                    if (ino >= 0 && ino < totalN) {
                        vert.nx = normais[ino * 3];
                        vert.ny = normais[ino * 3 + 1];
                        vert.nz = normais[ino * 3 + 2];
                    }
                } else {
                    vert.ny = 1.0f;
                }

                std::vector<ObjVertex> &lista = vertPorMat[materialAtual];
                const unsigned short novo = (unsigned short)lista.size();
                lista.push_back(vert);
                vistos[pedaco] = novo;
                cantos.push_back(novo);
            }

            /* Leque de triangulos: vale para qualquer face convexa. */
            std::vector<unsigned short> &indices = idxPorMat[materialAtual];
            for (size_t i = 2; i < cantos.size(); ++i) {
                indices.push_back(cantos[0]);
                indices.push_back(cantos[i - 1]);
                indices.push_back(cantos[i]);
            }
        }
    }

    for (std::map<std::string, std::vector<ObjVertex> >::iterator it = vertPorMat.begin();
         it != vertPorMat.end(); ++it) {
        const std::string &nome = it->first;
        std::vector<ObjVertex> &verts = it->second;
        std::vector<unsigned short> &indices = idxPorMat[nome];

        if (verts.empty() || indices.empty())
            continue;

        MeshGroup grupo;
        grupo.indexCount = (int)indices.size();

        std::map<std::string, ObjMaterial>::iterator mat = materiais.find(nome);
        if (mat != materiais.end()) {
            grupo.red = mat->second.red;
            grupo.green = mat->second.green;
            grupo.blue = mat->second.blue;

            if (!mat->second.texture.empty()) {
                bool ok = false;
                grupo.texture = loadTexture(pasta + mat->second.texture, ok);
                grupo.textured = ok;
            }
        }

        grupo.vbo = VBO::gen();
        grupo.ibo = IBO::gen();
        VBO::bind(grupo.vbo);
        VBO::uploadData(verts.size() * sizeof(ObjVertex), &verts[0]);
        IBO::bind(grupo.ibo);
        IBO::uploadData(indices.size() * sizeof(unsigned short), &indices[0]);
        VBO::unbind();
        IBO::unbind();

        static const VertexAttribute attribs[] = {
            { Shader::Position, 3, GL_FLOAT, (const GLvoid *)0 },
            { Shader::TexCoord, 3, GL_FLOAT, (const GLvoid *)(sizeof(float) * 3) },
            { Shader::Color,    2, GL_FLOAT, (const GLvoid *)(sizeof(float) * 6) }
        };
        grupo.vao.attr = attribs;
        grupo.vao.attrCount = 3;
        grupo.vao.vertSize = sizeof(ObjVertex);
        grupo.vao.vbo = grupo.vbo;
        grupo.vao.ibo = grupo.ibo;
        GLMeta::vaoInit(grupo.vao);

        groups.push_back(grupo);
    }

    char rastro[256];
    snprintf(rastro, sizeof(rastro), "OBJ: %s | grupos %d | escala 1/%.1f",
             path.c_str(), (int)groups.size(), unitsPerTile);
    prismTrace(rastro);

    return !groups.empty();
}

void Mesh::fini() {
    for (size_t i = 0; i < groups.size(); ++i) {
        GLMeta::vaoFini(groups[i].vao);
        IBO::del(groups[i].ibo);
        VBO::del(groups[i].vbo);
        if (groups[i].textured)
            TEX::del(groups[i].texture);
    }
    groups.clear();
}

int Renderer::loadMesh(const char *path, float unitsPerTile) {
    Mesh *malha = new Mesh();
    if (!malha->load(path, unitsPerTile)) {
        delete malha;
        return -1;
    }

    meshes.push_back(malha);
    return (int)meshes.size() - 1;
}

Renderer::Renderer() {
    /* O VAO so ganha conteudo em init(); ate la fica zerado, para fini() poder
       ser chamado sem medo mesmo se init() nunca correu. */
    vao.attr = 0;
    vao.attrCount = 0;
    vao.vertSize = 0;
    vao.nativeVAO = 0;
}
Renderer::~Renderer() { fini(); }

bool Renderer::init() {
    if (program)
        return true;

    const GLuint vertex = compile(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment)
        return false;

    program = gl.CreateProgram();
    gl.AttachShader(program, vertex);
    gl.AttachShader(program, fragment);
    /* Posicao e normal nos lugares que o motor ja usa para os atributos dele,
       para nao haver dois mapas de atributo concorrendo. */
    gl.BindAttribLocation(program, Shader::Position, "position");
    gl.BindAttribLocation(program, Shader::TexCoord, "normal");
    gl.BindAttribLocation(program, Shader::Color, "uv");
    gl.LinkProgram(program);

    GLint linked = 0;
    gl.GetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = {};
        gl.GetProgramInfoLog(program, sizeof(log) - 1, 0, log);
        std::fprintf(stderr, "Prism3D: programa nao ligou: %s\n", log);
        gl.DeleteProgram(program);
        program = 0;
        return false;
    }
    gl.DeleteShader(vertex);
    gl.DeleteShader(fragment);

    uniformModel = gl.GetUniformLocation(program, "model");
    uniformViewProjection = gl.GetUniformLocation(program, "viewProjection");
    uniformColor = gl.GetUniformLocation(program, "color");
    uniformTextured = gl.GetUniformLocation(program, "textured");

    /* A textura de um pixel branco. Existe para o caminho sem textura usar o
       mesmo shader: sem ela seriam dois programas, ou um ramo no shader, que
       e pior. */
    {
        const unsigned char branco[4] = { 255, 255, 255, 255 };
        blank = TEX::gen();
        TEX::bind(blank);
        TEX::uploadImage(1, 1, branco, GL_RGBA);
        TEX::setSmooth(false);
    }

    /* O cubo: 24 vertices, quatro por face, porque cada face tem a propria
       normal. Compartilhar os oito cantos daria normal media e o cubo sairia
       com aparencia de bola mal feita. */
    /* Mesmo formato da malha vinda de arquivo, para haver um shader so. O
       cubo nao tem textura, entao a UV fica em zero. */
    struct Vertex { float x, y, z, nx, ny, nz, u, v; };
    const Vertex vertices[24] = {
        /* frente (+z) */
        {-0.5f, -0.5f,  0.5f,  0, 0, 1}, { 0.5f, -0.5f,  0.5f,  0, 0, 1},
        { 0.5f,  0.5f,  0.5f,  0, 0, 1}, {-0.5f,  0.5f,  0.5f,  0, 0, 1},
        /* tras (-z) */
        { 0.5f, -0.5f, -0.5f,  0, 0, -1}, {-0.5f, -0.5f, -0.5f,  0, 0, -1},
        {-0.5f,  0.5f, -0.5f,  0, 0, -1}, { 0.5f,  0.5f, -0.5f,  0, 0, -1},
        /* direita (+x) */
        { 0.5f, -0.5f,  0.5f,  1, 0, 0}, { 0.5f, -0.5f, -0.5f,  1, 0, 0},
        { 0.5f,  0.5f, -0.5f,  1, 0, 0}, { 0.5f,  0.5f,  0.5f,  1, 0, 0},
        /* esquerda (-x) */
        {-0.5f, -0.5f, -0.5f, -1, 0, 0}, {-0.5f, -0.5f,  0.5f, -1, 0, 0},
        {-0.5f,  0.5f,  0.5f, -1, 0, 0}, {-0.5f,  0.5f, -0.5f, -1, 0, 0},
        /* topo (+y) */
        {-0.5f,  0.5f,  0.5f,  0, 1, 0}, { 0.5f,  0.5f,  0.5f,  0, 1, 0},
        { 0.5f,  0.5f, -0.5f,  0, 1, 0}, {-0.5f,  0.5f, -0.5f,  0, 1, 0},
        /* base (-y) */
        {-0.5f, -0.5f, -0.5f,  0, -1, 0}, { 0.5f, -0.5f, -0.5f,  0, -1, 0},
        { 0.5f, -0.5f,  0.5f,  0, -1, 0}, {-0.5f, -0.5f,  0.5f,  0, -1, 0},
    };

    GLushort indices[36];
    for (int face = 0; face < 6; ++face) {
        const GLushort base = (GLushort)(face * 4);
        const int at = face * 6;
        indices[at + 0] = base;
        indices[at + 1] = base + 1;
        indices[at + 2] = base + 2;
        indices[at + 3] = base;
        indices[at + 4] = base + 2;
        indices[at + 5] = base + 3;
    }
    indexCount = 36;

    vbo = VBO::gen();
    ibo = IBO::gen();

    VBO::bind(vbo);
    VBO::uploadData(sizeof(vertices), vertices);
    IBO::bind(ibo);
    IBO::uploadData(sizeof(indices), indices);
    VBO::unbind();
    IBO::unbind();

    /* O VAO vai pelo GLMeta do motor, e nao na mao, porque nem toda maquina
       tem VAO nativo: sem a extensao, o GLMeta cai para prender buffer e
       ponteiro de atributo a cada desenho, e o codigo daqui nao muda. */
    static const VertexAttribute attribs[] = {
        { Shader::Position, 3, GL_FLOAT, (const GLvoid *)0 },
        { Shader::TexCoord, 3, GL_FLOAT, (const GLvoid *)(sizeof(float) * 3) },
        { Shader::Color,    2, GL_FLOAT, (const GLvoid *)(sizeof(float) * 6) }
    };
    vao.attr = attribs;
    vao.attrCount = 3;
    vao.vertSize = sizeof(Vertex);
    vao.vbo = vbo;
    vao.ibo = ibo;
    GLMeta::vaoInit(vao);

    return true;
}

void Renderer::drawGroup(const MeshGroup &group, const Mat4 &model) {
    gl.UniformMatrix4fv(uniformModel, 1, GL_FALSE, model.m);
    gl.Uniform3f(uniformColor, group.red, group.green, group.blue);
    gl.Uniform1f(uniformTextured, group.textured ? 1.0f : 0.0f);

    gl.ActiveTexture(GL_TEXTURE0);
    TEX::bind(group.textured ? group.texture : blank);

    GLMeta::VAO &vaoRef = const_cast<GLMeta::VAO &>(group.vao);
    GLMeta::vaoBind(vaoRef);
    gl.DrawElements(GL_TRIANGLES, group.indexCount, GL_UNSIGNED_SHORT, 0);
    GLMeta::vaoUnbind(vaoRef);
}

void Renderer::fini() {
    for (size_t i = 0; i < meshes.size(); ++i) {
        meshes[i]->fini();
        delete meshes[i];
    }
    meshes.clear();
    placements.clear();

    if (!program)
        return;

    TEX::del(blank);

    GLMeta::vaoFini(vao);
    IBO::del(ibo);
    VBO::del(vbo);
    gl.DeleteProgram(program);
    program = 0;
}

void Renderer::setCamera(const Vec3 &newEye, const Vec3 &newTarget, float fovDegrees) {
    eye = newEye;
    target = newTarget;
    fov = fovDegrees;
    view = Mat4::lookAt(eye, target, Vec3(0, 1, 0));
    mapCamera = false;
}

void Renderer::setMapCamera(float scrollX, float scrollZ, float tilePixels,
                            float heightOnScreen) {
    mapScrollX = scrollX;
    mapScrollZ = scrollZ;
    mapTilePixels = tilePixels > 0.0f ? tilePixels : 32.0f;
    mapHeight = heightOnScreen;
    mapCamera = true;
}

void Renderer::draw(int width, int height, bool clearDepth) {
    if (!program || (boxes.empty() && placements.empty()))
        return;

    /*
     * Nao perguntamos o estado ao OpenGL, de proposito.
     *
     * A versao anterior fazia isto a cada quadro:
     *
     *     gl.IsEnabled(GL_DEPTH_TEST);
     *     gl.IsEnabled(GL_CULL_FACE);
     *     gl.GetBooleanv(GL_DEPTH_WRITEMASK, &mask);
     *
     * Toda chamada de consulta ao OpenGL obriga a CPU a esperar a GPU
     * terminar a fila para poder responder. Sao tres paradas por quadro para
     * perguntar algo que ja sabemos: profundidade e face traseira o motor
     * nunca liga, porque ele e um compositor 2D e o GLState dele nem conhece
     * essas propriedades (scene.h:88-104). Entao o estado a devolver e
     * constante, e restaurar as cegas custa zero.
     *
     * Se um dia o motor passar a usar profundidade por conta propria, isto
     * aqui quebra em silencio. E o preco, e esta escrito para quem vier.
     */

    if (clearDepth) {
        /* glClear respeita o teste de tesoura, e o ciclo de desenho do motor
           deixa a tesoura ligada na viewport corrente. Sem desligar, a limpeza
           sairia recortada. */
        glState.scissorTest.pushSet(false);
        gl.DepthMask(GL_TRUE);
        gl.Clear(GL_DEPTH_BUFFER_BIT);
        glState.scissorTest.pop();
    }

    if (depthEnabled) {
        gl.Enable(GL_DEPTH_TEST);
        gl.DepthFunc(GL_LESS);
        gl.DepthMask(GL_TRUE);
    } else {
        gl.Disable(GL_DEPTH_TEST);
    }

    gl.Enable(GL_CULL_FACE);
    gl.CullFace(GL_BACK);
    gl.FrontFace(GL_CCW);
    glState.blend.pushSet(false);

    Mat4 viewProjection;

    if (mapCamera)
    {
        /* Alcance folgado: o maior mapa do RPG Maker XP tem 500 tiles de
           lado, e a profundidade so precisa caber nele. */
        viewProjection = Mat4::mapOblique((float)width / mapTilePixels,
                                          (float)height / mapTilePixels,
                                          mapScrollX, mapScrollZ,
                                          mapHeight, 1024.0f);
    }
    else
    {
        const float aspect = height > 0 ? (float)width / (float)height : 1.0f;
        const Mat4 projection =
            Mat4::perspective(fov * 3.14159265f / 180.0f, aspect, 0.1f, 200.0f);
        viewProjection = projection * view;
    }

    glState.program.pushSet(program);
    gl.UniformMatrix4fv(uniformViewProjection, 1, GL_FALSE, viewProjection.m);
    gl.Uniform1i(gl.GetUniformLocation(program, "tex"), 0);

    /* As caixas, que nunca tem textura. */
    if (!boxes.empty()) {
        gl.Uniform1f(uniformTextured, 0.0f);
        gl.ActiveTexture(GL_TEXTURE0);
        TEX::bind(blank);
        GLMeta::vaoBind(vao);

        for (size_t i = 0; i < boxes.size(); ++i) {
            const Box &box = boxes[i];
            const Mat4 model = Mat4::translation(box.at) *
                               Mat4::rotationY(box.yaw) *
                               Mat4::scale(box.size);
            gl.UniformMatrix4fv(uniformModel, 1, GL_FALSE, model.m);
            gl.Uniform3f(uniformColor, box.red, box.green, box.blue);
            gl.DrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, 0);
        }

        GLMeta::vaoUnbind(vao);
    }

    /* Os modelos, um desenho por grupo de material. */
    for (size_t i = 0; i < placements.size(); ++i) {
        const Placement &posto = placements[i];
        if (posto.mesh < 0 || posto.mesh >= (int)meshes.size())
            continue;

        const Mat4 model = Mat4::translation(posto.at) *
                           Mat4::rotationY(posto.yaw) *
                           Mat4::scale(Vec3(posto.scale, posto.scale, posto.scale));

        Mesh *malha = meshes[posto.mesh];
        for (size_t g = 0; g < malha->groups.size(); ++g)
            drawGroup(malha->groups[g], model);
    }

    /* Devolve tudo ao que o compositor 2D espera encontrar. */
    glState.program.pop();
    glState.blend.pop();

    gl.Disable(GL_CULL_FACE);
    gl.DepthMask(GL_TRUE);
    gl.Disable(GL_DEPTH_TEST);
}

Element::Element(Scene &scene, int z)
    : SceneElement(scene, z) {
    const Scene::Geometry &geo = scene.getGeometry();
    width = geo.rect.w;
    height = geo.rect.h;
}

void Element::onGeometryChange(const Scene::Geometry &geo) {
    width = geo.rect.w;
    height = geo.rect.h;
}

void Element::draw() {
    theRenderer.draw(width, height);
}

} // namespace Prism3D
