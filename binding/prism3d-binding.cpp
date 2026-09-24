/*
** prism3d-binding.cpp
**
** O modulo Ruby do passo 3D do Prism.
**
** API do marco 1, de proposito pequena: comecar, mover a camera, empilhar
** caixas, limpar. Nada de arquivo de modelo, material, luz ou colisao. O
** caminhao do editor entra como as caixas que ele ja descreve, e so.
**
**   Prism3D.start(z = 0, viewport = nil)
**   Prism3D.camera(ex, ey, ez, ax, ay, az, fov)
**   Prism3D.add_box(x, y, z, larg, alt, prof, yaw, r, g, b)
**   Prism3D.clear
**   Prism3D.z / Prism3D.z=
**   Prism3D.visible / Prism3D.visible=
**   Prism3D.stop
**
** O elemento vive enquanto o jogo vive.
**
** Sem viewport ele entra na cena da tela. Com viewport ele entra na cena do
** proprio viewport, e isso muda tudo: no mkxp-z `Viewport` e ao mesmo tempo
** uma `Scene`, com lista propria de z, e um `SceneElement` na cena de cima.
** Os tiles e os personagens do Essentials vivem dentro do viewport do mapa,
** entao um elemento na cena da tela nunca disputa z com eles, disputa com o
** viewport inteiro, que desenha de uma vez. Para o objeto 3D passar atras de
** uma casa, ele precisa estar na mesma lista que ela.
*/

#include "binding-types.h"
#include "binding-util.h"
#include "debugwriter.h"
#include "prism-trace.h"
#include "exception.h"
#include <string>
#include "graphics.h"
#include "prism3d.h"
#include "viewport.h"
#include "sharedstate.h"

static Prism3D::Element *element = 0;

static Prism3D::Element *needElement() {
    if (!element)
        rb_raise(rb_eRuntimeError, "Prism3D.start precisa vir antes");

    return element;
}

RB_METHOD(prism3DStart) {
    RB_UNUSED_PARAM;

    int z = 0;
    VALUE viewportObj = Qnil;
    rb_get_args(argc, argv, "|io", &z, &viewportObj RB_ARG_END);

    Scene *cena = shState->graphics().getScreen();
    if (!NIL_P(viewportObj))
        cena = getPrivateDataCheck<Viewport>(viewportObj, ViewportType);

    prismTrace(NIL_P(viewportObj) ? "BINDING: start na cena da tela"
                                  : "BINDING: start dentro de um viewport");

    GFX_LOCK;
    if (!element) {
        element = new Prism3D::Element(*cena, z);
        if (!element->renderer().init()) {
            delete element;
            element = 0;
            GFX_UNLOCK;
            rb_raise(rb_eRuntimeError, "Prism3D: o renderizador nao subiu");
        }
    } else {
        element->setScene(*cena);
        element->setZ(z);
    }
    GFX_UNLOCK;

    return Qnil;
}

RB_METHOD(prism3DStop) {
    RB_UNUSED_PARAM;

    GFX_LOCK;
    delete element;
    element = 0;
    GFX_UNLOCK;

    return Qnil;
}

RB_METHOD(prism3DCamera) {
    RB_UNUSED_PARAM;

    double ex, ey, ez, ax, ay, az, fov = 45.0;
    rb_get_args(argc, argv, "ffffff|f", &ex, &ey, &ez, &ax, &ay, &az, &fov RB_ARG_END);

    GFX_LOCK;
    needElement()->renderer().setCamera(Prism3D::Vec3((float)ex, (float)ey, (float)ez),
                                        Prism3D::Vec3((float)ax, (float)ay, (float)az),
                                        (float)fov);
    GFX_UNLOCK;

    return Qnil;
}

/*
 * A camera do mapa.
 *
 *   Prism3D.map_camera(rolagem_x, rolagem_z, tile = 32, altura = 1.0)
 *
 * A rolagem vem em tiles, e e o canto superior esquerdo visivel. No Essentials
 * ela sai assim:
 *
 *   Prism3D.map_camera($game_map.display_x / 128.0, $game_map.display_y / 128.0)
 *
 * porque o display_x do RPG Maker conta em quartos de pixel: 32 pixels por
 * tile vezes 4 da 128.
 */
RB_METHOD(prism3DMapCamera) {
    RB_UNUSED_PARAM;

    double scrollX, scrollZ;
    double tile = 32.0, height = 1.0;
    rb_get_args(argc, argv, "ff|ff", &scrollX, &scrollZ, &tile, &height RB_ARG_END);

    GFX_LOCK;
    needElement()->renderer().setMapCamera((float)scrollX, (float)scrollZ,
                                           (float)tile, (float)height);
    GFX_UNLOCK;

    return Qnil;
}

/*
 * Troca a projecao paralela do mapa por perspectiva de verdade.
 *
 * `pitch` e a inclinacao a partir do horizonte, em graus. `fov` pequeno com
 * `distancia` grande tende a projecao paralela, entao perspectiva fraca sai
 * por construcao, sem misturar matriz.
 */
RB_METHOD(prism3DPerspective) {
    RB_UNUSED_PARAM;

    /* Distancia zero quer dizer "calcula", que e o caminho normal: o
       renderizador deriva a distancia do fov para a fileira do meio encostar
       nas duas bordas da tela. O padrao era 14.0, um valor chumbado que so
       existia para o primeiro ensaio, e quem chamasse com dois argumentos caia
       nele sem perceber: era isso que deixava a cena tres vezes maior que o
       devido, ate o Ruby passar -1.0 na mao para escapar. */
    double pitch = 60.0, fov = 30.0, distancia = 0.0;
    rb_get_args(argc, argv, "|fff", &pitch, &fov, &distancia RB_ARG_END);

    GFX_LOCK;
    needElement()->renderer().setMapPerspective((float)pitch, (float)fov,
                                                (float)distancia);
    GFX_UNLOCK;

    return Qnil;
}

RB_METHOD(prism3DPerspectiveOff) {
    RB_UNUSED_PARAM;

    GFX_LOCK;
    needElement()->renderer().setMapPerspectiveOff();
    GFX_UNLOCK;

    return Qnil;
}

/* Liga o plano de chao, que captura o mapa ja composto e o reprojeta. */
RB_METHOD(prism3DSetGround) {
    RB_UNUSED_PARAM;

    bool on;
    rb_get_args(argc, argv, "b", &on RB_ARG_END);

    GFX_LOCK;
    needElement()->renderer().setGround(on);
    GFX_UNLOCK;

    return rb_bool_new(on);
}

/*
 * Quanto o plano de chao passa do quadro capturado, em fracao da tela.
 *
 * Os dois valores nao tem resposta certa, e por isso viram botao: sobra demais
 * arrasta a borda da imagem, e sobre agua isso le como reflexo; sobra de menos
 * deixa faixa vazia no horizonte e cunha vazia nos cantos de baixo. Com o botao
 * aqui, achar o ponto certo e um ensaio em Ruby e nao uma recompilacao.
 */
RB_METHOD(prism3DGroundOvershoot) {
    RB_UNUSED_PARAM;

    double x = 0.05, z = 0.15;
    rb_get_args(argc, argv, "|ff", &x, &z RB_ARG_END);

    GFX_LOCK;
    needElement()->renderer().setGroundOvershoot((float)x, (float)z);
    GFX_UNLOCK;

    return Qnil;
}

/*
 * Onde um ponto do mundo cai na tela.
 *
 * Devolve [x, y, escala], em que a escala e quantos pixels vale uma unidade de
 * altura naquele ponto. E o que o Ruby precisa para o personagem continuar
 * sendo um cartao 2D, so que colocado pela camera 3D. Devolve nil se o ponto
 * estiver atras da camera.
 */
RB_METHOD(prism3DProject) {
    RB_UNUSED_PARAM;

    double x, y, z;
    rb_get_args(argc, argv, "fff", &x, &y, &z RB_ARG_END);

    Prism3D::Element *el = needElement();
    float sx = 0, sy = 0, escala = 0;

    GFX_LOCK;
    const bool ok = el->renderer().project((float)x, (float)y, (float)z,
                                           el->screenWidth(), el->screenHeight(),
                                           sx, sy, escala);
    GFX_UNLOCK;

    if (!ok)
        return Qnil;

    VALUE saida = rb_ary_new2(3);
    rb_ary_push(saida, rb_float_new(sx));
    rb_ary_push(saida, rb_float_new(sy));
    rb_ary_push(saida, rb_float_new(escala));
    return saida;
}

RB_METHOD(prism3DAddBox) {
    RB_UNUSED_PARAM;

    double x, y, z, w, h, d;
    double yaw = 0.0, red = 1.0, green = 1.0, blue = 1.0;
    rb_get_args(argc, argv, "ffffff|ffff",
                &x, &y, &z, &w, &h, &d, &yaw, &red, &green, &blue RB_ARG_END);

    Prism3D::Box box;
    box.at = Prism3D::Vec3((float)x, (float)y, (float)z);
    box.size = Prism3D::Vec3((float)w, (float)h, (float)d);
    box.yaw = (float)yaw;
    box.red = (float)red;
    box.green = (float)green;
    box.blue = (float)blue;

    GFX_LOCK;
    needElement()->renderer().add(box);
    GFX_UNLOCK;

    return Qnil;
}

/*
 * Carrega um modelo de arquivo e devolve o indice dele.
 *
 *   id = Prism3D.load_model("Graphics/Models/lab/lab.obj", 16.0)
 *
 * O segundo argumento e quantas unidades do arquivo valem uma celula do mapa.
 * Modelo extraido de jogo de DS costuma vir com 16. Devolve nil se o arquivo
 * nao abriu ou nao tinha geometria, em vez de levantar excecao: o jogo deve
 * continuar rodando sem o objeto.
 */
RB_METHOD(prism3DLoadModel) {
    RB_UNUSED_PARAM;

    const char *path;
    double unitsPerTile = 16.0;
    rb_get_args(argc, argv, "z|f", &path, &unitsPerTile RB_ARG_END);

    prismTrace((std::string("BINDING: load_model ") + path).c_str());

    /*
     * A fronteira com o Ruby nao pode deixar excecao de C++ passar: o
     * interpretador nao sabe o que fazer com ela e o processo cai sem
     * mensagem. Carregar modelo mexe com arquivo, e arquivo falha de muitos
     * jeitos, entao aqui vira nil e o jogo segue sem o objeto.
     */
    int id = -1;
    GFX_LOCK;
    try {
        id = needElement()->renderer().loadMesh(path, (float)unitsPerTile);
    } catch (const Exception &e) {
        prismTrace((std::string("BINDING: load_model falhou: ") + e.msg).c_str());
        id = -1;
    } catch (...) {
        prismTrace("BINDING: load_model falhou por motivo desconhecido");
        id = -1;
    }
    GFX_UNLOCK;

    prismTrace(id < 0 ? "BINDING: load_model devolveu nil"
                      : "BINDING: load_model devolveu um indice");

    return id < 0 ? Qnil : rb_fix_new(id);
}

/*
 *   Prism3D.add_model(id, x, y, z, yaw = 0.0, escala = 1.0)
 *
 * A posicao esta em celulas do mapa, com y para cima. O giro esta em radianos.
 */
RB_METHOD(prism3DAddModel) {
    RB_UNUSED_PARAM;

    int id;
    double x, y, z;
    double yaw = 0.0, scale = 1.0;
    rb_get_args(argc, argv, "ifff|ff", &id, &x, &y, &z, &yaw, &scale RB_ARG_END);

    Prism3D::Placement posto;
    posto.mesh = id;
    posto.at = Prism3D::Vec3((float)x, (float)y, (float)z);
    posto.yaw = (float)yaw;
    posto.scale = (float)scale;

    GFX_LOCK;
    needElement()->renderer().addPlacement(posto);
    GFX_UNLOCK;

    return Qnil;
}

RB_METHOD(prism3DClear) {
    RB_UNUSED_PARAM;

    GFX_LOCK;
    needElement()->renderer().clear();
    GFX_UNLOCK;

    return Qnil;
}

RB_METHOD(prism3DCount) {
    RB_UNUSED_PARAM;

    GFX_LOCK;
    const size_t count = needElement()->renderer().count();
    GFX_UNLOCK;

    return rb_fix_new((long)count);
}

RB_METHOD(prism3DGetZ) {
    RB_UNUSED_PARAM;

    GFX_LOCK;
    const int z = needElement()->getZ();
    GFX_UNLOCK;

    return rb_fix_new(z);
}

RB_METHOD(prism3DSetZ) {
    RB_UNUSED_PARAM;

    int z;
    rb_get_args(argc, argv, "i", &z RB_ARG_END);

    GFX_LOCK;
    needElement()->setZ(z);
    GFX_UNLOCK;

    return rb_fix_new(z);
}

RB_METHOD(prism3DGetVisible) {
    RB_UNUSED_PARAM;

    GFX_LOCK;
    const bool visible = needElement()->getVisible();
    GFX_UNLOCK;

    return rb_bool_new(visible);
}

RB_METHOD(prism3DSetVisible) {
    RB_UNUSED_PARAM;

    bool visible;
    rb_get_args(argc, argv, "b", &visible RB_ARG_END);

    GFX_LOCK;
    needElement()->setVisible(visible);
    GFX_UNLOCK;

    return rb_bool_new(visible);
}

/* So para o ensaio: desligar a profundidade prova que e ela quem ordena. */
RB_METHOD(prism3DSetDepth) {
    RB_UNUSED_PARAM;

    bool on;
    rb_get_args(argc, argv, "b", &on RB_ARG_END);

    GFX_LOCK;
    needElement()->renderer().depthEnabled = on;
    GFX_UNLOCK;

    return rb_bool_new(on);
}

void prism3DBindingInit() {
    Debug() << "Prism3D: registrando o modulo Ruby";
    VALUE module = rb_define_module("Prism3D");

    _rb_define_module_function(module, "start", prism3DStart);
    _rb_define_module_function(module, "stop", prism3DStop);
    _rb_define_module_function(module, "camera", prism3DCamera);
    _rb_define_module_function(module, "map_camera", prism3DMapCamera);
    _rb_define_module_function(module, "perspective", prism3DPerspective);
    _rb_define_module_function(module, "perspective_off", prism3DPerspectiveOff);
    _rb_define_module_function(module, "ground=", prism3DSetGround);
    _rb_define_module_function(module, "ground_overshoot", prism3DGroundOvershoot);
    _rb_define_module_function(module, "project", prism3DProject);
    _rb_define_module_function(module, "add_box", prism3DAddBox);
    _rb_define_module_function(module, "load_model", prism3DLoadModel);
    _rb_define_module_function(module, "add_model", prism3DAddModel);
    _rb_define_module_function(module, "clear", prism3DClear);
    _rb_define_module_function(module, "count", prism3DCount);
    _rb_define_module_function(module, "z", prism3DGetZ);
    _rb_define_module_function(module, "z=", prism3DSetZ);
    _rb_define_module_function(module, "visible", prism3DGetVisible);
    _rb_define_module_function(module, "visible=", prism3DSetVisible);
    _rb_define_module_function(module, "depth_test=", prism3DSetDepth);
}
