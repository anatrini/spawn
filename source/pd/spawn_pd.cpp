/*
    spawn - run a command line from Pd (vanilla and plugdata) and read what it prints.

    A message such as
        python -u script.py --port 9001
    starts the command in the folder of the patch; stop ends it.
    Left outlet: each line the command prints (stdout and stderr).
    Right outlet: 1 when the command starts, 0 when it ends.
    Deleting the object or closing the patch stops the command.
*/

#include "m_pd.h"

#include "proc.h"

#include <cstring>
#include <string>
#include <vector>

static const double kPollMs = 10;  // how often the output is checked while a command runs

typedef struct _spawn {
    t_object x_obj;
    t_outlet* out_text;
    t_outlet* out_running;
    t_clock* clock;
    t_symbol* dir;      // folder of the patch
    spawn::Proc* proc;
    bool waiting;       // a command was started and its end has not been output yet
    bool stopping;      // stop was sent: the end is expected
} t_spawn;

static t_class* spawn_class;

static void spawn_output_line(t_spawn* x, const std::string& line)
{
    if (line.empty())
        return;
    // A line with Pd separators is output whole, as one symbol.
    if (line.find_first_of(",;\\$") != std::string::npos) {
        outlet_anything(x->out_text, gensym(line.c_str()), 0, 0);
        return;
    }
    t_binbuf* b = binbuf_new();
    binbuf_text(b, line.c_str(), line.size());
    int ac = binbuf_getnatom(b);
    t_atom* av = binbuf_getvec(b);
    if (ac > 0) {
        if (av[0].a_type == A_SYMBOL)
            outlet_anything(x->out_text, av[0].a_w.w_symbol, ac - 1, av + 1);
        else
            outlet_list(x->out_text, &s_list, ac, av);
    }
    binbuf_free(b);
}

// Pd's main thread: outputs what the reader thread has queued.
static void spawn_tick(t_spawn* x)
{
    std::vector<spawn::Event> events;
    x->proc->drain(events);
    for (const auto& e : events) {
        if (e.kind == spawn::Event::Line) {
            spawn_output_line(x, e.line);
        }
        else {
            if (x->stopping || e.kind == spawn::Event::Signal)
                post("spawn: command stopped");
            else if (e.code != 0)
                pd_error(x, "spawn: command ended with error code %d", e.code);
            x->waiting = false;
            outlet_float(x->out_running, 0);
        }
    }
    if (x->waiting)
        clock_delay(x->clock, kPollMs);
}

static void spawn_anything(t_spawn* x, t_symbol* s, int argc, t_atom* argv)
{
    std::string command;
    if (s && s != &s_list && s != &s_float)
        command = s->s_name;
    for (int i = 0; i < argc; ++i) {
        char buf[MAXPDSTRING];
        atom_string(argv + i, buf, MAXPDSTRING);
        if (!command.empty())
            command += ' ';
        command += buf;
    }
    if (command.empty())
        return;

    std::string error;
    if (!x->proc->start(command, x->dir->s_name, error)) {
        pd_error(x, "spawn: %s", error.c_str());
        return;
    }
    x->stopping = false;
    x->waiting = true;
    outlet_float(x->out_running, 1);
    clock_delay(x->clock, kPollMs);
}

static void spawn_stop(t_spawn* x)
{
    if (x->proc->running())
        x->stopping = true;
    x->proc->stop();
}

static void* spawn_new(void)
{
    t_spawn* x = (t_spawn*)pd_new(spawn_class);
    x->out_text = outlet_new(&x->x_obj, 0);
    x->out_running = outlet_new(&x->x_obj, &s_float);
    x->clock = clock_new(x, (t_method)spawn_tick);
    x->dir = canvas_getdir(canvas_getcurrent());
    x->proc = new spawn::Proc(nullptr);
    x->waiting = false;
    x->stopping = false;
    return x;
}

static void spawn_free(t_spawn* x)
{
    delete x->proc;  // stops the command and waits for it
    clock_free(x->clock);
}

extern "C" void spawn_setup(void)
{
    spawn_class = class_new(gensym("spawn"), (t_newmethod)spawn_new, (t_method)spawn_free,
                            sizeof(t_spawn), CLASS_DEFAULT, A_NULL);
    class_addmethod(spawn_class, (t_method)spawn_stop, gensym("stop"), A_NULL);
    class_addlist(spawn_class, (t_method)spawn_anything);
    class_addanything(spawn_class, (t_method)spawn_anything);
}
