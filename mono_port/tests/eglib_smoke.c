/* =====================================================================
 *  eglib_smoke.c - Phase 0 smoke test for the freestanding eglib core
 * ---------------------------------------------------------------------
 *  Links against libmono_port.a with -nostdlib.  Exercises the eglib
 *  containers/strings that Mono's interpreter leans on, and reports
 *  through pal_console_putc.
 *
 *  Two console backends:
 *    HOST build  -> Linux int 0x80 write(1,..)  (runs directly in WSL)
 *    MINIOS build-> MiniOS ring-3 int 0x80 SYS_WRITE(4) fd=1 -> serial
 *  Both happen to be "int 0x80 with eax=4, ebx=fd, ecx=buf, edx=len",
 *  which is why the same stub serves both.
 * ===================================================================== */

#include <glib.h>

extern void (*pal_console_putc)(char);

/* ---- raw write(2) / exit(2) via int 0x80 (no libc) ---- */
static int sys_write(int fd, const void *buf, int len)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(4), "b"(fd), "c"(buf), "d"(len)
                         : "memory");
    return r;
}
static void sys_exit(int code)
{
    __asm__ __volatile__("int $0x80" :: "a"(1), "b"(code));
    for (;;) { }
}

static void con_putc(char c) { sys_write(1, &c, 1); }
static void out(const char *s) { while (*s) con_putc(*s++); }
static void out_num(long v)
{
    char b[24]; int i = 0;
    if (v < 0) { con_putc('-'); v = -v; }
    if (!v) { con_putc('0'); return; }
    while (v) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) con_putc(b[--i]);
}

static int g_fails = 0;
static void check(const char *what, int ok)
{
    out(ok ? "  [ok]   " : "  [FAIL] ");
    out(what);
    con_putc('\n');
    if (!ok) g_fails++;
}

/* ---- the actual exercises ---- */
static void test_gstring(void)
{
    GString *s = g_string_new("mono");
    g_string_append(s, "-port");
    g_string_append_c(s, ':');
    g_string_append(s, " phase0");
    check("GString append", strcmp(s->str, "mono-port: phase0") == 0);
    /* force several realloc rounds - this is what caught the broken
       bump-realloc that discarded old contents */
    for (int i = 0; i < 500; i++) g_string_append(s, "0123456789");
    check("GString grow x500", s->len == 17 + 5000 && s->str[17 + 4999] == '9'
                               && strncmp(s->str, "mono-port", 9) == 0);
    g_string_free(s, TRUE);
}

static void test_gptrarray(void)
{
    GPtrArray *a = g_ptr_array_new();
    for (long i = 0; i < 300; i++) g_ptr_array_add(a, (gpointer)(i + 1));
    check("GPtrArray len", a->len == 300);
    check("GPtrArray data", (long)a->pdata[0] == 1 && (long)a->pdata[299] == 300);
    g_ptr_array_free(a, TRUE);
}

static void test_ghashtable(void)
{
    GHashTable *h = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(h, (gpointer)"System.Object", (gpointer)"klass1");
    g_hash_table_insert(h, (gpointer)"System.String", (gpointer)"klass2");
    g_hash_table_insert(h, (gpointer)"System.Int32",  (gpointer)"klass3");
    const char *v = (const char *)g_hash_table_lookup(h, "System.String");
    check("GHashTable size", g_hash_table_size(h) == 3);
    check("GHashTable lookup", v && strcmp(v, "klass2") == 0);
    check("GHashTable miss", g_hash_table_lookup(h, "System.Nope") == NULL);
    g_hash_table_destroy(h);
}

static void test_lists(void)
{
    GSList *l = NULL;
    for (long i = 0; i < 50; i++) l = g_slist_prepend(l, (gpointer)(i + 1));
    check("GSList length", g_slist_length(l) == 50);
    check("GSList head", (long)l->data == 50);
    g_slist_free(l);

    GList *d = NULL;
    d = g_list_append(d, (gpointer)11);
    d = g_list_append(d, (gpointer)22);
    check("GList append", g_list_length(d) == 2 && (long)g_list_last(d)->data == 22);
    g_list_free(d);
}

static void test_strutil(void)
{
    char *j = g_strdup_printf("%s/%d", "asm", 42);
    check("g_strdup_printf", j && strcmp(j, "asm/42") == 0);
    g_free(j);

    gchar **parts = g_strsplit("a:b:c", ":", -1);
    check("g_strsplit", parts && parts[0] && parts[2] &&
                        strcmp(parts[0], "a") == 0 && strcmp(parts[2], "c") == 0 &&
                        parts[3] == NULL);
    g_strfreev(parts);

    char *up = g_ascii_strup("mono", -1);
    check("g_ascii_strup", up && strcmp(up, "MONO") == 0);
    g_free(up);
}

static void test_utf8(void)
{
    /* "A" U+4E2D "z"  ->  3 codepoints */
    const char *u = "A\xE4\xB8\xAD" "z";
    glong items = 0;
    gunichar2 *w = g_utf8_to_utf16(u, -1, NULL, &items, NULL);
    check("g_utf8_to_utf16 len", w && items == 3);
    check("g_utf8_to_utf16 cp", w && w[0] == 'A' && w[1] == 0x4E2D && w[2] == 'z');
    if (w) {
        gchar *back = g_utf16_to_utf8(w, items, NULL, NULL, NULL);
        check("g_utf16_to_utf8 rt", back && strcmp(back, u) == 0);
        g_free(back);
        g_free(w);
    }
}

int _start_c(void)
{
    pal_console_putc = con_putc;

    out("=== eglib freestanding smoke (mono_port Phase 0) ===\n");
    test_gstring();
    test_gptrarray();
    test_ghashtable();
    test_lists();
    test_strutil();
    test_utf8();

    out("=== ");
    if (g_fails == 0) out("ALL PASS");
    else { out("FAILURES: "); out_num(g_fails); }
    out(" ===\n");
    return g_fails;
}

/* no libc -> supply the ELF entry point ourselves */
__asm__(".globl _start\n_start:\n\tcall _start_c\n\tmovl %eax, %ebx\n\tmovl $1, %eax\n\tint $0x80\n");

void pal_unused_keep_exit(void) { sys_exit(0); }
