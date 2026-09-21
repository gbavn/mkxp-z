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

/* O chao e a imagem do mapa ja pronta, com a arte e a sombra que o tileset
   tem. Iluminar de novo escureceria o desenho do jogo. */
uniform float unlit;

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
       binaria, e misturar exigiria ordenar por profundidade a cada quadro.
       No chao nao vale: o alfa do alvo copiado nao quer dizer nada. */
    if (unlit < 0.5 && alpha < 0.5)
        discard;

    float sombra = mix(0.45 + 0.55 * lambert, 1.0, unlit);
    gl_FragColor = vec4(base * sombra, 1.0);
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
        prismTrace((std::string("SHADER nao compilou: ") + log).c_str());
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
        /* O ultimo argumento e `freeOnClose`, e ele PRECISA ser false aqui.
           Com true o motor instala SDL_RWopsCloseFree como fechamento, e essa
           funcao chama SDL_FreeRW no ponteiro: como `ops` esta na pilha, o
           alocador receberia um endereco de pilha e o processo morreria sem
           exececao, sem caixa de erro e sem minidump. O unico lugar do motor
           que passa true e o font.cpp, e la o RWops vem de SDL_AllocRW. */
        shState->fileSystem().openReadRaw(ops, path.c_str(), false);
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

    /* Rastro dos dois lados do fechamento: e ele que prova por onde o processo
       passou, em vez de deixar a conclusao por deducao. Sai quando o carregador
       estiver de pe. */
    prismTrace("OBJ: antes do SDL_RWclose");
    SDL_RWclose(&ops);
    prismTrace("OBJ: depois do SDL_RWclose");

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
        /* false pelo mesmo motivo do readWholeFile: `ops` esta na pilha. O
           IMG_LoadTyped_RW abaixo fecha o RWops por conta propria, pelo `1` do
           freesrc, inclusive quando a decodificacao falha. */
        shState->fileSystem().openReadRaw(ops, path.c_str(), false);
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
    prismTrace((std::string("OBJ: comecando ") + path).c_str());

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
        prismTrace((std::string("SHADER nao ligou: ") + log).c_str());
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
    uniformUnlit = gl.GetUniformLocation(program, "unlit");

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

    /*
     * O quadrado do chao, no plano Y zero, de zero a um nos dois eixos.
     *
     * A UV ja vai embutida e invertida no eixo vertical de proposito: a
     * textura copiada do alvo tem a origem no canto de baixo, como todo
     * framebuffer de OpenGL, enquanto a fileira de cima da tela e o Z menor
     * do mundo. Sem essa inversao o mapa sairia espelhado no sentido norte
     * e sul.
     */
    {
        /*
         * O plano passa um pouco do quadro capturado, de proposito.
         *
         * O chao fica inclinado em relacao a camera, entao a profundidade dele
         * encurta na projecao e sobra faixa vazia em cima e embaixo. Medindo
         * com inclinacao de 65 graus, a sobra e de uns 19 pixels de cada lado.
         * Esticar o plano e deixar a textura grampeada na borda preenche isso
         * arrastando a fileira de fora, o que le como chao continuando. Faixa
         * preta perto do jogador leria como defeito.
         */
        const float sobraZ = 0.15f, sobraX = 0.05f;
        struct Vertex { float x, y, z, nx, ny, nz, u, v; };
        const Vertex quad[4] = {
            { -sobraX, 0, -sobraZ,  0, 1, 0,  -sobraX, 1 + sobraZ },
            { 1 + sobraX, 0, -sobraZ,  0, 1, 0,  1 + sobraX, 1 + sobraZ },
            { 1 + sobraX, 0, 1 + sobraZ,  0, 1, 0,  1 + sobraX, -sobraZ },
            { -sobraX, 0, 1 + sobraZ,  0, 1, 0,  -sobraX, -sobraZ },
        };
        const GLushort idx[6] = { 0, 1, 2, 0, 2, 3 };

        groundVbo = VBO::gen();
        groundIbo = IBO::gen();
        VBO::bind(groundVbo);
        VBO::uploadData(sizeof(quad), quad);
        IBO::bind(groundIbo);
        IBO::uploadData(sizeof(idx), idx);
        VBO::unbind();
        IBO::unbind();

        static const VertexAttribute quadAttribs[] = {
            { Shader::Position, 3, GL_FLOAT, (const GLvoid *)0 },
            { Shader::TexCoord, 3, GL_FLOAT, (const GLvoid *)(sizeof(float) * 3) },
            { Shader::Color,    2, GL_FLOAT, (const GLvoid *)(sizeof(float) * 6) }
        };
        groundVao.attr = quadAttribs;
        groundVao.attrCount = 3;
        groundVao.vertSize = sizeof(Vertex);
        groundVao.vbo = groundVbo;
        groundVao.ibo = groundIbo;
        GLMeta::vaoInit(groundVao);
    }

    prismTrace("RENDER: shader e cubo prontos");
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
    if (groundTexW > 0)
        TEX::del(groundTex);
    groundTexW = groundTexH = 0;

    GLMeta::vaoFini(groundVao);
    IBO::del(groundIbo);
    VBO::del(groundVbo);

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

void Renderer::setMapPerspective(float pitchDegrees, float fovDegrees,
                                 float distance) {
    /* 89 e nao 90: a prumo o alvo fica em cima do olho e o vetor de cima fica
       paralelo a direcao de visao, o que quebra o lookAt. */
    perspPitch = pitchDegrees < 1.0f ? 1.0f
               : (pitchDegrees > 89.0f ? 89.0f : pitchDegrees);
    perspFov = fovDegrees > 0.5f ? fovDegrees : 0.5f;
    /* Zero ou menos quer dizer automatica, que e o caso normal. */
    perspDistance = distance;
    mapPerspective = true;
}

/*
** A matriz da camera do mapa, uma so para tudo.
**
** Chao, objeto e `project` precisam concordar na virgula, entao nenhum deles
** monta matriz por conta propria: os tres chamam daqui.
*/
Mat4 Renderer::mapViewProjection(int width, int height) const {
    const float tilesWide = (float)width / mapTilePixels;
    const float tilesHigh = (float)height / mapTilePixels;

    if (!mapPerspective) {
        /* Alcance folgado: o maior mapa do RPG Maker XP tem 500 tiles de
           lado, e a profundidade so precisa caber nele. */
        return Mat4::mapOblique(tilesWide, tilesHigh,
                                mapScrollX, mapScrollZ, mapHeight, 1024.0f);
    }

    /* O alvo e o centro da area visivel do mapa, no chao. A camera fica ao sul
       dele, que e o Z maior, e acima. Olhar do sul e o que poe o horizonte no
       alto da tela e mantem o norte ao fundo, como no mapa 2D. */
    const Vec3 alvo(mapScrollX + tilesWide * 0.5f, 0.0f,
                    mapScrollZ + tilesHigh * 0.5f);

    const float aspect = height > 0 ? (float)width / (float)height : 1.0f;
    const float meioFov = perspFov * 0.5f * 3.14159265f / 180.0f;

    /*
     * A distancia sai do campo de visao, e nao de um numero solto.
     *
     * O eixo X do mapa fica perpendicular a direcao de visao, porque a camera
     * so se inclina, nunca gira de lado. Entao ele nao sofre encurtamento, e a
     * largura visivel na altura do alvo e 2 * d * tan(fov/2) * proporcao.
     * Igualando isso a largura do quadro, a fileira do meio encosta nas duas
     * bordas da tela em qualquer campo de visao.
     *
     * E isso que faz perspectiva fraca funcionar como botao: com `fov` pequeno
     * a camera se afasta sozinha e a imagem tende a de hoje, em vez de o chao
     * encolher no meio da tela.
     */
    float distancia = perspDistance;
    if (distancia <= 0.0f)
        distancia = tilesWide / (2.0f * std::tan(meioFov) * aspect);

    const float rad = perspPitch * 3.14159265f / 180.0f;
    const Vec3 olho(alvo.x,
                    alvo.y + distancia * std::sin(rad),
                    alvo.z + distancia * std::cos(rad));

    /* O plano de fundo acompanha a distancia, senao camera longe, que e o que
       perspectiva fraca pede, corta o mapa inteiro. */
    const Mat4 projecao = Mat4::perspective(perspFov * 3.14159265f / 180.0f,
                                            aspect, 0.1f, distancia * 4.0f + 64.0f);
    return projecao * Mat4::lookAt(olho, alvo, Vec3(0, 1, 0));
}

bool Renderer::project(float x, float y, float z, int width, int height,
                       float &outX, float &outY, float &outScale) const {
    const Mat4 m = mapViewProjection(width, height);

    /* Duas projecoes: o ponto, e o mesmo ponto uma unidade mais alto. A
       distancia entre os dois na tela e a escala ali, que e o que o sprite
       precisa para encolher com a distancia. */
    float px[2], py[2];
    for (int i = 0; i < 2; ++i) {
        const float vy = y + (float)i;
        const float cx = m.m[0]*x + m.m[4]*vy + m.m[8]*z  + m.m[12];
        const float cy = m.m[1]*x + m.m[5]*vy + m.m[9]*z  + m.m[13];
        const float cw = m.m[3]*x + m.m[7]*vy + m.m[11]*z + m.m[15];

        if (cw <= 0.0001f)
            return false;

        px[i] = (cx / cw + 1.0f) * 0.5f * (float)width;
        py[i] = (1.0f - cy / cw) * 0.5f * (float)height;
    }

    outX = px[0];
    outY = py[0];
    outScale = py[0] - py[1];
    return true;
}

void Renderer::ensureGroundTexture(int width, int height) {
    if (groundTexW == width && groundTexH == height)
        return;

    if (groundTexW > 0)
        TEX::del(groundTex);

    groundTex = TEX::gen();
    TEX::bind(groundTex);
    TEX::allocEmpty(width, height);
    /* Filtro suave aqui, ao contrario do resto: o chao e o unico lugar em que
       a imagem e esticada de verdade pela perspectiva, e vizinho mais proximo
       viraria escada nas fileiras do fundo. */
    TEX::setSmooth(true);
    TEX::setRepeat(false);

    groundTexW = width;
    groundTexH = height;
}

/*
** Captura o mapa ja composto e redesenha ele como um plano.
**
** Isto so funciona por causa da ordem de z da cena. O TilemapRenderer do
** Essentials da z zero ao tile de prioridade zero, que e o chao, e z crescente
** ao resto. Como a cena compoe em ordem de z, um elemento em z 1 desenha num
** instante em que so o chao foi pintado. A separacao entre chao e coisa alta
** sai da propria ordenacao, sem reimplementar tilemap nenhum.
**
** O preco e que limpar a cor apaga tambem o que estiver abaixo de z zero,
** como reflexo e panorama.
*/
void Renderer::drawGround(int width, int height) {
    ensureGroundTexture(width, height);

    TEX::bind(groundTex);
    gl.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);

    /* glClear respeita a tesoura, e o ciclo de desenho deixa ela ligada. */
    glState.scissorTest.pushSet(false);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    glState.scissorTest.pop();

    const float tilesWide = (float)width / mapTilePixels;
    const float tilesHigh = (float)height / mapTilePixels;

    const Mat4 model = Mat4::translation(Vec3(mapScrollX, 0.0f, mapScrollZ)) *
                       Mat4::scale(Vec3(tilesWide, 1.0f, tilesHigh));

    /* Sem descarte de face: o plano e um so, e errar o lado dele por causa da
       ordem dos vertices sumiria com o chao inteiro sem dizer por que. */
    gl.Disable(GL_CULL_FACE);

    gl.UniformMatrix4fv(uniformModel, 1, GL_FALSE, model.m);
    gl.Uniform3f(uniformColor, 1.0f, 1.0f, 1.0f);
    gl.Uniform1f(uniformTextured, 1.0f);
    gl.Uniform1f(uniformUnlit, 1.0f);
    gl.ActiveTexture(GL_TEXTURE0);
    TEX::bind(groundTex);

    GLMeta::vaoBind(groundVao);
    gl.DrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
    GLMeta::vaoUnbind(groundVao);

    gl.Uniform1f(uniformUnlit, 0.0f);
    gl.Enable(GL_CULL_FACE);
}

void Renderer::draw(int width, int height, bool clearDepth) {
    if (!program)
        return;

    /* O chao e desenho por si so: ele aparece mesmo sem objeto nenhum na
       cena. */
    const bool comChao = groundEnabled && mapCamera && mapPerspective;
    if (boxes.empty() && placements.empty() && !comChao)
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
        viewProjection = mapViewProjection(width, height);
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
    gl.Uniform1f(uniformUnlit, 0.0f);

    /* O chao vem antes de tudo: ele e o fundo em que o resto pisa. */
    if (comChao)
        drawGround(width, height);

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
