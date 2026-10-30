// The Switch's Vulkan build (SCDA_GPU=vk) links neither EGL nor libelf, yet two archives refer to them:
//   libSDL2.a's EGL glue (SDL_egl.o): no SDL window is opened, Vulkan presents to the default NWindow itself
//   (switch_platform.cpp), so none of it runs; each call fails as EGL without a display would
//   mesa-switch's NVK: libelf serves its CUBIN parser, which never runs on the Switch
#include <stddef.h>

typedef void* EGLDisplay;
typedef unsigned int EGLBoolean;
typedef int EGLint;

#define EGL_NOT_INITIALIZED 0x3001

EGLint eglGetError(void) { return EGL_NOT_INITIALIZED; }
EGLDisplay eglGetDisplay(void* id) { (void)id; return NULL; }
EGLDisplay eglGetPlatformDisplay(unsigned platform, void* id, const void* attribs) {
    (void)platform; (void)id; (void)attribs;
    return NULL;
}
EGLBoolean eglInitialize(EGLDisplay d, EGLint* major, EGLint* minor) { (void)d; (void)major; (void)minor; return 0; }
EGLBoolean eglTerminate(EGLDisplay d) { (void)d; return 0; }
const char* eglQueryString(EGLDisplay d, EGLint name) { (void)d; (void)name; return NULL; }
void* eglGetProcAddress(const char* name) { (void)name; return NULL; }
EGLBoolean eglBindAPI(unsigned api) { (void)api; return 0; }
unsigned eglQueryAPI(void) { return 0; }
EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint* attribs, void** configs, EGLint size, EGLint* n) {
    (void)d; (void)attribs; (void)configs; (void)size;
    if (n) *n = 0;
    return 0;
}
EGLBoolean eglGetConfigAttrib(EGLDisplay d, void* config, EGLint attrib, EGLint* value) {
    (void)d; (void)config; (void)attrib; (void)value;
    return 0;
}
void* eglCreateContext(EGLDisplay d, void* config, void* share, const EGLint* attribs) {
    (void)d; (void)config; (void)share; (void)attribs;
    return NULL;
}
EGLBoolean eglDestroyContext(EGLDisplay d, void* ctx) { (void)d; (void)ctx; return 0; }
void* eglCreateWindowSurface(EGLDisplay d, void* config, void* win, const EGLint* attribs) {
    (void)d; (void)config; (void)win; (void)attribs;
    return NULL;
}
void* eglCreatePbufferSurface(EGLDisplay d, void* config, const EGLint* attribs) {
    (void)d; (void)config; (void)attribs;
    return NULL;
}
EGLBoolean eglDestroySurface(EGLDisplay d, void* surface) { (void)d; (void)surface; return 0; }
EGLBoolean eglMakeCurrent(EGLDisplay d, void* draw, void* read, void* ctx) {
    (void)d; (void)draw; (void)read; (void)ctx;
    return 0;
}
EGLBoolean eglSwapBuffers(EGLDisplay d, void* surface) { (void)d; (void)surface; return 0; }
EGLBoolean eglSwapInterval(EGLDisplay d, EGLint interval) { (void)d; (void)interval; return 0; }
EGLBoolean eglWaitGL(void) { return 0; }
EGLBoolean eglWaitNative(EGLint engine) { (void)engine; return 0; }

unsigned int elf_version(unsigned int v) { (void)v; return 0; }
void* elf_memory(char* image, size_t size) { (void)image; (void)size; return NULL; }
int elf_end(void* elf) { (void)elf; return 0; }
int elf_kind(void* elf) { (void)elf; return 0; }
void* elf64_getehdr(void* elf) { (void)elf; return NULL; }
void* elf64_getshdr(void* scn) { (void)scn; return NULL; }
void* elf_getscn(void* elf, size_t idx) { (void)elf; (void)idx; return NULL; }
void* elf_nextscn(void* elf, void* scn) { (void)elf; (void)scn; return NULL; }
void* elf_getdata(void* scn, void* data) { (void)scn; (void)data; return NULL; }
int elf_getshdrstrndx(void* elf, size_t* dst) {
    (void)elf;
    if (dst) *dst = 0;
    return -1;
}
char* elf_strptr(void* elf, size_t sec, size_t off) { (void)elf; (void)sec; (void)off; return NULL; }
int elf_errno(void) { return 0; }
const char* elf_errmsg(int err) { (void)err; return "no libelf on the Switch"; }
