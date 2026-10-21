/**
    spawn - run a command line from Max and read what it prints.

    A message such as
        python -u script.py --port 9001
    starts the command in the folder of the patch; stop ends it.
    Left outlet: each line the command prints (stdout and stderr).
    Right outlet: 1 when the command starts, 0 when it ends.
    Deleting the object or closing the patch stops the command.
*/

#include "ext.h"
#include "ext_obex.h"
#include "jpatcher_api.h"

#include "proc.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

typedef struct _spawn {
    t_object ob;
    void* out_text;
    void* out_running;
    void* qelem;
    t_object* patcher;
    spawn::Proc* proc;
    bool stopping;  // stop was sent: the end is expected
} t_spawn;

static t_class* spawn_class;

void* spawn_new(t_symbol* s, long argc, t_atom* argv);
void spawn_free(t_spawn* x);
void spawn_assist(t_spawn* x, void* b, long m, long a, char* s);
void spawn_anything(t_spawn* x, t_symbol* s, long argc, t_atom* argv);
void spawn_stop(t_spawn* x);
void spawn_output(t_spawn* x);

void ext_main(void* r)
{
    t_class* c = class_new("spawn", (method)spawn_new, (method)spawn_free, (long)sizeof(t_spawn),
                           0L, A_GIMME, 0);
    class_addmethod(c, (method)spawn_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)spawn_anything, "list", A_GIMME, 0);
    class_addmethod(c, (method)spawn_stop, "stop", 0);
    class_addmethod(c, (method)spawn_assist, "assist", A_CANT, 0);
    class_register(CLASS_BOX, c);
    spawn_class = c;
}

void* spawn_new(t_symbol* s, long argc, t_atom* argv)
{
    t_spawn* x = (t_spawn*)object_alloc(spawn_class);
    if (!x)
        return NULL;
    x->out_running = intout(x);
    x->out_text = outlet_new(x, NULL);
    x->qelem = qelem_new(x, (method)spawn_output);
    x->patcher = NULL;
    x->stopping = false;
    object_obex_lookup(x, gensym("#P"), &x->patcher);
    void* q = x->qelem;
    x->proc = new spawn::Proc([q] { qelem_set(q); });
    return x;
}

void spawn_free(t_spawn* x)
{
    delete x->proc;  // stops the command and waits for it
    qelem_free(x->qelem);
}

void spawn_assist(t_spawn* x, void* b, long m, long a, char* s)
{
    if (m == ASSIST_INLET)
        snprintf_zero(s, 256, "Command line to run, stop");
    else if (a == 0)
        snprintf_zero(s, 256, "Lines printed by the command");
    else
        snprintf_zero(s, 256, "1 while the command runs, 0 when it ends");
}

// Folder of the patch that contains the object (or of the first saved patch
// above it); the home folder if the patch has never been saved.
static std::string patch_folder(t_spawn* x)
{
    for (t_object* p = x->patcher; p; p = jpatcher_get_parentpatcher(p)) {
        t_symbol* fp = jpatcher_get_filepath(p);
        if (fp && fp->s_name[0]) {
            char native[MAX_PATH_CHARS];
            path_nameconform(fp->s_name, native, PATH_STYLE_NATIVE, PATH_TYPE_BOOT);
            std::string path(native);
            size_t slash = path.rfind('/');
            if (slash != std::string::npos)
                return path.substr(0, slash);
        }
    }
    const char* home = std::getenv("HOME");
    return home ? home : "";
}

void spawn_anything(t_spawn* x, t_symbol* s, long argc, t_atom* argv)
{
    std::vector<t_atom> atoms;
    if (s != gensym("list")) {
        t_atom a;
        atom_setsym(&a, s);
        atoms.push_back(a);
    }
    atoms.insert(atoms.end(), argv, argv + argc);
    if (atoms.empty())
        return;

    long size = 0;
    char* text = NULL;
    atom_gettext((long)atoms.size(), atoms.data(), &size, &text, OBEX_UTIL_ATOM_GETTEXT_NOESCAPE);
    std::string command = text ? text : "";
    sysmem_freeptr(text);

    std::string error;
    if (!x->proc->start(command, patch_folder(x), error)) {
        object_error((t_object*)x, "%s", error.c_str());
        return;
    }
    x->stopping = false;
    outlet_int(x->out_running, 1);
}

void spawn_stop(t_spawn* x)
{
    if (x->proc->running())
        x->stopping = true;
    x->proc->stop();
}

// Main thread: outputs what the reader thread has queued.
void spawn_output(t_spawn* x)
{
    std::vector<spawn::Event> events;
    x->proc->drain(events);
    for (const auto& e : events) {
        if (e.kind == spawn::Event::Line) {
            if (e.line.empty())
                continue;
            // A line with Max separators is output whole, as one symbol.
            if (e.line.find_first_of(",;\\$") != std::string::npos) {
                outlet_anything(x->out_text, gensym(e.line.c_str()), 0, NULL);
                continue;
            }
            long ac = 0;
            t_atom* av = NULL;
            atom_setparse(&ac, &av, e.line.c_str());
            if (ac > 0) {
                if (atom_gettype(av) == A_SYM)
                    outlet_anything(x->out_text, atom_getsym(av), ac - 1, av + 1);
                else
                    outlet_list(x->out_text, NULL, (short)ac, av);
            }
            sysmem_freeptr(av);
        }
        else {
            if (x->stopping || e.kind == spawn::Event::Signal)
                object_post((t_object*)x, "command stopped");
            else if (e.code != 0)
                object_warn((t_object*)x, "command ended with error code %d", e.code);
            outlet_int(x->out_running, 0);
        }
    }
}
