// Workaround for macOS 15.4+: dyld no longer fills legacy __DATA,__dyld sections.
// Steam's steamloader.dylib relies on it for Rocksmith 2014's DRM bootstrap.
// This dylib must be injected BEFORE steamloader.dylib.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <libgen.h>
#include <stdarg.h>

static FILE *logf_;
static void logmsg(const char *fmt, const char *a, void *p) {
    if (!logf_) return;
    fprintf(logf_, fmt, a, p); fflush(logf_);
}
void rsfix_log(const char *fmt, ...) {
    if (!logf_) return;
    va_list ap; va_start(ap, fmt); vfprintf(logf_, fmt, ap); va_end(ap); fflush(logf_);
}

static int fixed_dyld_func_lookup(const char *name, void **address) {
    void *f = NULL;
    if (name && strncmp(name, "__dyld_", 7) == 0) {
        char buf[256];
        snprintf(buf, sizeof buf, "_dyld_%s", name + 7);   // __dyld_image_count -> _dyld_image_count
        f = dlsym(RTLD_DEFAULT, buf);
        if (!f) f = dlsym(RTLD_DEFAULT, name + 7);        // __dyld_dlopen -> dlopen
        if (!f) f = dlsym(RTLD_DEFAULT, name + 1);        // __dyld_x -> _dyld_x (C name _dyld_x)
    } else if (name) {
        f = dlsym(RTLD_DEFAULT, name[0] == '_' ? name + 1 : name);
    }
    logmsg("lookup %s -> %p\n", name ? name : "(null)", f);
    if (address) *address = f;
    return f != NULL;
}

static void fixed_lazy_binder(void) {
    logmsg("%s%p\n", "unexpected call to dyld_lazy_symbol_binding_entry_point ", NULL);
    abort();
}

__attribute__((constructor)) static void init(void) {
    Dl_info info;
    if (dladdr((void *)init, &info) && info.dli_fname) {
        char dir[1024], p[1100];
        strncpy(dir, info.dli_fname, sizeof dir - 1); dir[sizeof dir - 1] = 0;
        snprintf(p, sizeof p, "%s/dyldfix.log", dirname(dir));
        logf_ = fopen(p, "w");
    }
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        const char *path = _dyld_get_image_name(i);
        if (!path || !strstr(path, "steamloader.dylib")) continue;
        unsigned long size = 0;
        void **sect = (void **)getsectiondata((const struct mach_header_64 *)_dyld_get_image_header(i),
                                              "__DATA", "__dyld", &size);
        if (!sect || size < 16) { logmsg("no __dyld section in %s%p\n", path, NULL); continue; }
        vm_protect(mach_task_self(), (vm_address_t)sect & ~(vm_address_t)0xfff, 0x2000, 0,
                   VM_PROT_READ | VM_PROT_WRITE);
        logmsg("patching %s __dyld at %p\n", path, sect);
        sect[0] = (void *)fixed_lazy_binder;
        sect[1] = (void *)fixed_dyld_func_lookup;
    }
}
