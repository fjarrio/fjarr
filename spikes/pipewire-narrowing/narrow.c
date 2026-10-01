/* Spike (throwaway): can an ordinary session-user process narrow its own PipeWire connection to
 * one node, hand the descriptor over, and is the receiver then unable to widen it again?
 *   narrow <node> [exec cmd...]   connect, narrow to {core, node}, hide everything else, run cmd
 *                                 with the connection's fd as $PW_FD (and fd 3); default: probe
 *   probe                         on fd $PW_FD: list what is visible, try to re-grant everything,
 *                                 list again */
#include <pipewire/pipewire.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct pw_main_loop *loop;
static int seen;
static void on_global(void *d, uint32_t id, uint32_t perms, const char *type, uint32_t v, const struct spa_dict *props) {
    const char *name = props ? spa_dict_lookup(props, "node.name") : NULL;
    const char *cls = props ? spa_dict_lookup(props, "media.class") : NULL;
    printf("  global %u %s perms=%c%c%c%c %s %s\n", id, type, perms & PW_PERM_R ? 'r' : '-', perms & PW_PERM_W ? 'w' : '-',
           perms & PW_PERM_X ? 'x' : '-', perms & PW_PERM_M ? 'm' : '-', name ? name : "", cls ? cls : "");
    seen++;
}
static const struct pw_registry_events reg_events = {PW_VERSION_REGISTRY_EVENTS, .global = on_global};
static void on_done(void *d, uint32_t id, int seq) { if (id == PW_ID_CORE) pw_main_loop_quit(loop); }
static void on_error(void *d, uint32_t id, int seq, int res, const char *msg) { printf("  core error id=%u res=%d %s\n", id, res, msg); }
static const struct pw_core_events core_events = {PW_VERSION_CORE_EVENTS, .done = on_done, .error = on_error};

static void roundtrip(struct pw_core *core) {
    pw_core_sync(core, PW_ID_CORE, 0);
    pw_main_loop_run(loop);
}

static void list(struct pw_core *core, const char *label) {
    struct spa_hook l;
    seen = 0;
    printf("%s:\n", label);
    struct pw_registry *reg = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    spa_zero(l);
    pw_registry_add_listener(reg, &l, &reg_events, NULL);
    roundtrip(core);
    printf("  (%d globals)\n", seen);
    spa_hook_remove(&l);
    pw_proxy_destroy((struct pw_proxy *)reg);
}

int main(int argc, char **argv) {
    pw_init(&argc, &argv);
    loop = pw_main_loop_new(NULL);
    struct pw_context *ctx = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
    struct spa_hook core_l;
    if (argc >= 3 && !strcmp(argv[1], "narrow")) {
        uint32_t node = atoi(argv[2]);
        uint32_t factory = getenv("PW_CLIENT_NODE_FACTORY") ? atoi(getenv("PW_CLIENT_NODE_FACTORY")) : PW_ID_ANY;
        struct pw_core *core = pw_context_connect(ctx, NULL, 0);
        if (!core) { perror("connect"); return 1; }
        spa_zero(core_l);
        pw_core_add_listener(core, &core_l, &core_events, NULL);
        struct pw_permission p[4];
        int n = 0;
        p[n++] = PW_PERMISSION_INIT(PW_ID_CORE, PW_PERM_R | PW_PERM_X);
        p[n++] = PW_PERMISSION_INIT(node, PW_PERM_R | PW_PERM_X);
        if (factory != PW_ID_ANY) p[n++] = PW_PERMISSION_INIT(factory, PW_PERM_R | PW_PERM_X); /* a stream is a client-node */
        p[n++] = PW_PERMISSION_INIT(PW_ID_ANY, 0);
        int r = getenv("PW_NO_NARROW") ? 0 : pw_client_update_permissions(pw_core_get_client(core), n, p); /* the control */
        roundtrip(core);
        printf("narrowed to core + node %u: update_permissions=%d\n", node, r);
        int fd = pw_core_steal_fd(core);
        printf("stolen fd %d\n", fd);
        if (fd != 3) { dup2(fd, 3); close(fd); fd = 3; }
        char buf[16];
        snprintf(buf, sizeof buf, "%d", fd);
        setenv("PW_FD", buf, 1);
        fflush(stdout);
        if (argc > 3) execvp(argv[3], argv + 3);
        execl("/proc/self/exe", argv[0], "probe", NULL);
        perror("exec");
        return 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "probe")) {
        int fd = atoi(getenv("PW_FD") ? getenv("PW_FD") : "3");
        struct pw_core *core = pw_context_connect_fd(ctx, fd, NULL, 0);
        if (!core) { perror("connect_fd"); return 1; }
        spa_zero(core_l);
        pw_core_add_listener(core, &core_l, &core_events, NULL);
        list(core, "visible through the handed connection");
        struct pw_permission all[] = {PW_PERMISSION_INIT(PW_ID_ANY, PW_PERM_ALL)};
        int r = pw_client_update_permissions(pw_core_get_client(core), 1, all);
        roundtrip(core);
        printf("receiver tried to re-grant itself everything: update_permissions=%d\n", r);
        list(core, "visible after the attempt");
        return 0;
    }
    fprintf(stderr, "usage: narrow <node> [cmd...] | probe\n");
    return 2;
}
