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

#include <cstdio>
#include <cstring>

#include "gl-fun.h"
#include "gl-util.h"
#include "glstate.h"
#include "graphics.h"
#include "shader.h"
#include "sharedstate.h"
#include "vertex.h"

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

uniform mat4 model;
uniform mat4 viewProjection;

varying vec3 vNormal;

void main() {
    vNormal = mat3(model[0].xyz, model[1].xyz, model[2].xyz) * normal;
    gl_Position = viewProjection * model * vec4(position, 1.0);
}
)";

static const char *fragmentSource = R"(
#ifdef GLSLES
precision mediump float;
#endif

uniform vec3 color;
varying vec3 vNormal;

void main() {
    /* Iluminacao de uma luz so, fixa. Serve para as faces se distinguirem:
       sem isso o cubo vira uma silhueta chapada e ninguem consegue dizer se
       ele girou. */
    vec3 light = normalize(vec3(-0.4, 0.9, 0.55));
    float lambert = max(dot(normalize(vNormal), light), 0.0);
    gl_FragColor = vec4(color * (0.45 + 0.55 * lambert), 1.0);
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

    /* O cubo: 24 vertices, quatro por face, porque cada face tem a propria
       normal. Compartilhar os oito cantos daria normal media e o cubo sairia
       com aparencia de bola mal feita. */
    struct Vertex { float x, y, z, nx, ny, nz; };
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
        { Shader::TexCoord, 3, GL_FLOAT, (const GLvoid *)(sizeof(float) * 3) }
    };
    vao.attr = attribs;
    vao.attrCount = 2;
    vao.vertSize = sizeof(Vertex);
    vao.vbo = vbo;
    vao.ibo = ibo;
    GLMeta::vaoInit(vao);

    return true;
}

void Renderer::fini() {
    if (!program)
        return;

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
}

void Renderer::draw(int width, int height, bool clearDepth) {
    if (!program || boxes.empty())
        return;

    /* Profundidade e face traseira o GLState do motor nao conhece, entao
       salvamos na mao. Mistura e programa ele conhece, e vao por push/pop,
       senao o cache dele ficaria mentindo sobre o estado real. */
    const GLboolean hadDepthTest = gl.IsEnabled(GL_DEPTH_TEST);
    const GLboolean hadCullFace = gl.IsEnabled(GL_CULL_FACE);
    GLboolean hadDepthMask = GL_TRUE;
    gl.GetBooleanv(GL_DEPTH_WRITEMASK, &hadDepthMask);

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

    const float aspect = height > 0 ? (float)width / (float)height : 1.0f;
    const Mat4 projection =
        Mat4::perspective(fov * 3.14159265f / 180.0f, aspect, 0.1f, 200.0f);
    const Mat4 viewProjection = projection * view;

    glState.program.pushSet(program);
    gl.UniformMatrix4fv(uniformViewProjection, 1, GL_FALSE, viewProjection.m);
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

    /* Devolve tudo. A ordem importa: VAO por ultimo, senao um bind de buffer
       do motor cairia dentro do nosso VAO. */
    GLMeta::vaoUnbind(vao);
    glState.program.pop();
    glState.blend.pop();

    if (!hadCullFace)
        gl.Disable(GL_CULL_FACE);
    gl.DepthMask(hadDepthMask);
    if (!hadDepthTest)
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
