/*
** prism3d-binding.cpp
**
** O modulo Ruby do passo 3D do Prism.
**
** API do marco 1, de proposito pequena: comecar, mover a camera, empilhar
** caixas, limpar. Nada de arquivo de modelo, material, luz ou colisao. O
** caminhao do editor entra como as caixas que ele ja descreve, e so.
**
**   Prism3D.start(z = 0)
**   Prism3D.camera(ex, ey, ez, ax, ay, az, fov)
**   Prism3D.add_box(x, y, z, larg, alt, prof, yaw, r, g, b)
**   Prism3D.clear
**   Prism3D.z / Prism3D.z=
**   Prism3D.visible / Prism3D.visible=
**   Prism3D.stop
**
** O elemento vive enquanto o jogo vive. Ele entra na cena da tela, que o
** Graphics ja expoe por getScreen(), entao nada aqui precisa de estado global
** novo no motor.
*/

#include "binding-util.h"
#include "graphics.h"
#include "prism3d.h"
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
    rb_get_args(argc, argv, "|i", &z RB_ARG_END);

    GFX_LOCK;
    if (!element) {
        element = new Prism3D::Element(*shState->graphics().getScreen(), z);
        if (!element->renderer().init()) {
            delete element;
            element = 0;
            GFX_UNLOCK;
            rb_raise(rb_eRuntimeError, "Prism3D: o renderizador nao subiu");
        }
    } else {
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
    VALUE module = rb_define_module("Prism3D");

    _rb_define_module_function(module, "start", prism3DStart);
    _rb_define_module_function(module, "stop", prism3DStop);
    _rb_define_module_function(module, "camera", prism3DCamera);
    _rb_define_module_function(module, "add_box", prism3DAddBox);
    _rb_define_module_function(module, "clear", prism3DClear);
    _rb_define_module_function(module, "count", prism3DCount);
    _rb_define_module_function(module, "z", prism3DGetZ);
    _rb_define_module_function(module, "z=", prism3DSetZ);
    _rb_define_module_function(module, "visible", prism3DGetVisible);
    _rb_define_module_function(module, "visible=", prism3DSetVisible);
    _rb_define_module_function(module, "depth_test=", prism3DSetDepth);
}
